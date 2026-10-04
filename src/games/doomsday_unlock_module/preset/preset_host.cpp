#include "preset/preset_host.h"
#include "doomsday_embedded.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace preset {
namespace {

using gtajson::Json;

double NowMonotonic() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The predicate/base library shared with the simulator runtime: extract
// BASE_LIB from the embedded src/lua_runtime.js exactly like
// preset_runner.base_lib().
std::string BaseLib(std::string* error) {
  const auto embedded = doomsday_embedded::Find("lua_runtime.js");
  if (!embedded.empty()) {
    const std::string source(embedded);
    const std::string marker = "const BASE_LIB = `";
    const auto start = source.find(marker);
    const auto bodyStart = start == std::string::npos ? 0 : start + marker.size();
    const auto end = start == std::string::npos ? std::string::npos : source.find("`;", bodyStart);
    if (end != std::string::npos) return source.substr(bodyStart, end - bodyStart);
  }
  *error = "cannot extract BASE_LIB from embedded lua_runtime.js";
  return "";
}

// ---- Lua <-> Json ----

Json LuaToJson(lua_State* L, int idx) {
  const int t = lua_type(L, idx);
  switch (t) {
    case LUA_TNIL:
      return Json::Null();
    case LUA_TBOOLEAN:
      return Json::Bool(lua_toboolean(L, idx) != 0);
    case LUA_TNUMBER:
      return Json::Number(lua_tonumber(L, idx));
    case LUA_TSTRING: {
      const char* s = lua_tostring(L, idx);
      return Json::String(s ? s : "");
    }
    case LUA_TTABLE: {
      // sequence part first, then keyed members (string keys only)
      bool isSequence = true;
      const lua_Integer n = lua_rawlen(L, idx);
      if (n > 0) {
        for (lua_Integer i = 1; i <= n; ++i) {
          if (lua_rawgeti(L, idx, i) == LUA_TNIL) {
            lua_pop(L, 1);
            isSequence = false;
            break;
          }
          lua_pop(L, 1);
        }
      }
      Json out = isSequence ? Json::Array() : Json::Object();
      if (isSequence) {
        for (lua_Integer i = 1; i <= n; ++i) {
          lua_rawgeti(L, idx, i);
          out.Push(LuaToJson(L, -1));
          lua_pop(L, 1);
        }
        return out;
      }
      lua_pushnil(L);
      while (lua_next(L, idx) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
          out.Set(lua_tostring(L, -2), LuaToJson(L, -1));
        }
        lua_pop(L, 1);
      }
      return out;
    }
    default:
      return Json::Null();
  }
}

void PushJson(lua_State* L, const Json& j) {
  switch (j.type) {
    case Json::Type::Null:
      lua_pushnil(L);
      break;
    case Json::Type::Bool:
      lua_pushboolean(L, j.boolean ? 1 : 0);
      break;
    case Json::Type::Number:
      lua_pushnumber(L, j.number);
      break;
    case Json::Type::String:
      lua_pushstring(L, j.string.c_str());
      break;
    case Json::Type::Array:
      lua_createtable(L, 0, 0);
      for (std::size_t i = 0; i < j.items.size(); ++i) {
        PushJson(L, j.items[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
      }
      break;
    case Json::Type::Object:
      lua_createtable(L, 0, static_cast<int>(j.members.size()));
      for (const auto& [k, v] : j.members) {
        PushJson(L, v);
        lua_setfield(L, -2, k.c_str());
      }
      break;
  }
}

constexpr double kTurnTolCoarse = 3.5;

}  // namespace

PresetLog::PresetLog(const std::string& path) {
  out_.open(path, std::ios::app);
}

void PresetLog::Event(const std::string& kind, const Json& fields) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!out_) return;
  Json record = Json::Object();
  record.Set("kind", Json::String(kind));
  record.Set("t", Json::Number(NowMonotonic()));
  for (const auto& [k, v] : fields.members) record.Set(k, v);
  out_ << record.Dump() << "\n";
  out_.flush();
}

void PresetLog::Frame(const Json& record) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!out_) return;
  out_ << record.Dump() << "\n";
  out_.flush();
}

PresetRunner::PresetRunner(HostContext* context, motion::PresetMotion* motion,
                           const matcher::MatchResult& binding,
                           std::vector<std::string> wait_lookaheads,
                           const std::string& log_path)
    : context_(context),
      motion_(motion),
      wait_lookaheads_(std::move(wait_lookaheads)),
      log_(log_path) {
  const vision::SceneModel* scene = context->Scene();
  for (const auto& [index_text, authored] : binding.mirrors) {
    const int index = index_text;
    if (index >= 0 && index < static_cast<int>(scene->mirrors.size())) {
      const std::string visual = scene->mirrors[index].id;
      mirror_visual_[authored] = visual;
      mirror_authored_[visual] = authored;
    }
  }
  for (const auto& [index_text, authored] : binding.targets) {
    const int index = index_text;
    if (index >= 0 && index < static_cast<int>(scene->targets.size())) {
      target_visual_[authored] = scene->targets[index].id;
      target_authored_[scene->targets[index].id] = authored;
    }
  }
}

PresetRunner::~PresetRunner() {
  stop_requested_ = true;
  if (thread_.joinable()) thread_.join();
  if (L_) lua_close(L_);
}

std::vector<std::string> PresetRunner::ScanWaitLookaheads(const std::string& code) {
  // port of scan_wait_lookaheads: the Kth wait() maps to the next
  // rotate / rotate_interval_until call after it (straight-line scan)
  std::vector<std::string> lookaheads;
  int pending = -1;
  std::istringstream in(code);
  std::string rawLine;
  while (std::getline(in, rawLine)) {
    std::string line = rawLine;
    const auto comment = line.find("--");
    if (comment != std::string::npos) line = line.substr(0, comment);
    // trim
    const auto firstNonSpace = line.find_first_not_of(" \t");
    if (firstNonSpace == std::string::npos) continue;
    line = line.substr(firstNonSpace);
    const auto waitPos = line.find("wait(");
    const bool isWait = waitPos != std::string::npos &&
                        (waitPos == 0 || !std::isalnum(static_cast<unsigned char>(line[waitPos - 1])));
    if (isWait) {
      lookaheads.push_back("");
      pending = static_cast<int>(lookaheads.size()) - 1;
      continue;
    }
    std::size_t rotPos = line.find("rotate_interval_until(");
    if (rotPos == std::string::npos) rotPos = line.find("rotate(");
    if (rotPos != std::string::npos && pending >= 0) {
      const auto quote1 = line.find('"', rotPos);
      if (quote1 != std::string::npos) {
        const auto quote2 = line.find('"', quote1 + 1);
        if (quote2 != std::string::npos) {
          lookaheads[pending] = line.substr(quote1 + 1, quote2 - quote1 - 1);
          pending = -1;
        }
      }
    }
  }
  return lookaheads;
}

const tracking::TrackedMirror* PresetRunner::TrackedMirror(
    const std::string& visual) const {
  const tracking::TrackingFrame* tracked = context_->LastTracked();
  if (tracked == nullptr) return nullptr;
  for (const auto& m : tracked->mirrors) {
    if (m.mirror_id == visual) return &m;
  }
  return nullptr;
}

bool PresetRunner::MirrorCenter(const std::string& authored, double* cx,
                                double* cy) const {
  const auto it = mirror_visual_.find(authored);
  if (it == mirror_visual_.end()) return false;
  const vision::SceneModel* scene = context_->Scene();
  if (scene == nullptr) return false;
  for (const auto& m : scene->mirrors) {
    if (m.id == it->second) {
      *cx = m.cx;
      *cy = m.cy;
      return true;
    }
  }
  return false;
}

double PresetRunner::MirrorAngle(const std::string& authored) const {
  const auto it = mirror_visual_.find(authored);
  if (it == mirror_visual_.end()) return 0.0;
  const tracking::TrackedMirror* m = TrackedMirror(it->second);
  if (m == nullptr || !m->has_angle || m->misses != 0) return 0.0;
  return m->angle_deg;
}

Json PresetRunner::BuildObs(lua_State* L) const {
  // mirrors table
  Json mirrors = Json::Object();
  for (const auto& [authored, visual] : mirror_visual_) {
    double angle = 0.0;
    bool selected = false;
    const tracking::TrackedMirror* m = TrackedMirror(visual);
    if (m != nullptr) {
      if (m->has_angle && m->misses == 0) {
        angle = std::round(m->angle_deg * 1000.0) / 1000.0;
      }
      selected = m->selected;
    }
    Json entry = Json::Object();
    entry.Set("angle", Json::Number(angle));
    entry.Set("selected", Json::Bool(selected));
    mirrors.Set(authored, entry);
  }
  Json entities = Json::Object();
  const vision::SceneModel* scene = context_->Scene();
  if (scene != nullptr) {
    for (const auto& t : scene->targets) {
      const auto it = target_authored_.find(t.id);
      const std::string authored = it == target_authored_.end() ? t.id : it->second;
      Json entry = Json::Object();
      entry.Set("alive", Json::Bool(t.alive));
      entry.Set("heat", Json::Number(0.0));
      entities.Set(authored, entry);
    }
  }
  Json beam = Json::Object();
  beam.Set("chain", Json::Object());
  beam.Set("terminal", Json::Null());
  beam.Set("lastMirror", Json::Null());
  Json obs = Json::Object();
  obs.Set("mirrors", mirrors);
  obs.Set("entities", entities);
  obs.Set("beam", beam);
  return obs;
}

void PresetRunner::SetCurrentOp(const std::string& name, const OpPending& pending) {
  current_op_name_ = name;
  current_op_args_ = pending.args_text;
}

void PresetRunner::OnWaitStart() {
  const std::size_t index = wait_index_++;
  if (index >= wait_lookaheads_.size()) return;
  const std::string& authored = wait_lookaheads_[index];
  if (authored.empty()) return;
  double cx = 0, cy = 0;
  const auto it = mirror_visual_.find(authored);
  if (it == mirror_visual_.end()) return;
  if (!MirrorCenter(authored, &cx, &cy)) return;
  if (motion_->current_selected() == it->second) return;
  auto job = std::make_shared<motion::MotionJob>();
  job->kind = "select";
  job->mirror_id = it->second;
  job->has_center = true;
  job->center_x = cx;
  job->center_y = cy;
  motion_->Submit(job);
}

double TimeoutOf(const std::vector<Json>& args, std::size_t index,
                 double fallback) {
  if (args.size() > index) {
    const Json& opts = args[index];
    if (const Json* t = opts.Find("timeout"); t != nullptr && t->IsNumber()) {
      return t->number;
    }
  }
  return fallback;
}

std::shared_ptr<OpPending> PresetRunner::RotateStart(
    const std::vector<Json>& args) {
  auto pending = std::make_shared<OpPending>();
  if (args.empty() || args[0].type != Json::Type::String ||
      args.size() < 2 || args[1].type != Json::Type::Number) {
    pending->failed = "rotate 需要 (id, 角度)";
    return pending;
  }
  const std::string mirrorId = args[0].string;
  const double deg = args[1].number;
  double cx = 0, cy = 0;
  if (!MirrorCenter(mirrorId, &cx, &cy)) {
    pending->failed = "未知镜子: " + mirrorId;
    return pending;
  }
  char argsText[64];
  std::snprintf(argsText, sizeof(argsText), "\"%s\", %g", mirrorId.c_str(), deg);
  pending->args_text = argsText;
  pending->timeout = 25.0;
  pending->rotate = std::make_shared<OpPending::RotateState>();
  pending->rotate->id = mirrorId;
  pending->rotate->deg = std::fmod(std::fmod(deg, 180.0) + 180.0, 180.0);
  pending->rotate->phase = "select";
  return pending;
}

void PresetRunner::RotateStep(OpPending* pending, double dt) {
  (void)dt;
  auto& state = *pending->rotate;
  if (state.phase == "select") {
    if (state.job == nullptr) {
      double cx = 0, cy = 0;
      MirrorCenter(state.id, &cx, &cy);
      auto job = std::make_shared<motion::MotionJob>();
      job->kind = "select";
      job->mirror_id = mirror_visual_[state.id];
      job->has_center = true;
      job->center_x = cx;
      job->center_y = cy;
      state.job = motion_->Submit(job);
      return;
    }
    if (!state.job->failed.empty()) {
      pending->failed = "select " + state.id + ": " + state.job->failed;
      return;
    }
    if (state.job->done) {
      state.phase = "turn";
      state.job.reset();
    }
    return;
  }
  if (state.phase == "turn") {
    if (state.job == nullptr) {
      auto job = std::make_shared<motion::MotionJob>();
      job->kind = "turn";
      job->mirror_id = mirror_visual_[state.id];
      job->target_deg = state.deg;
      job->tol_deg = kTurnTolCoarse;
      state.job = motion_->Submit(job);
      return;
    }
    if (!state.job->failed.empty()) {
      pending->failed = "turn " + state.id + ": " + state.job->failed;
      return;
    }
    if (state.job->done) pending->done = true;
  }
}

std::shared_ptr<OpPending> PresetRunner::RotateIntervalStart(
    const std::vector<Json>& args) {
  auto pending = std::make_shared<OpPending>();
  if (args.size() < 3 || args[0].type != Json::Type::String ||
      args[1].type != Json::Type::Number || args[2].type != Json::Type::Number) {
    pending->failed = "rotate_interval_until 需要 (id, 角度1, 角度2, 条件)";
    return pending;
  }
  const std::string mirrorId = args[0].string;
  const double deg1 = args[1].number;
  const double deg2 = args[2].number;
  double cx = 0, cy = 0;
  if (!MirrorCenter(mirrorId, &cx, &cy)) {
    pending->failed = "未知镜子: " + mirrorId;
    return pending;
  }
  const double timeout = TimeoutOf(args, 4, 30.0);
  char argsText[96];
  std::snprintf(argsText, sizeof(argsText), "\"%s\", %g, %g", mirrorId.c_str(),
                deg1, deg2);
  pending->args_text = argsText;
  pending->timeout = timeout;
  pending->interval = std::make_shared<OpPending::IntervalState>();
  pending->interval->id = mirrorId;
  pending->interval->deg1 = std::fmod(std::fmod(deg1, 180.0) + 180.0, 180.0);
  pending->interval->deg2 = std::fmod(std::fmod(deg2, 180.0) + 180.0, 180.0);
  pending->interval->target = pending->interval->deg1;
  pending->interval->phase = "select";
  pending->interval->timeout = timeout;
  return pending;
}

void PresetRunner::RotateIntervalStep(OpPending* pending, double dt) {
  auto& state = *pending->interval;
  state.elapsed += dt;
  if (state.elapsed >= state.timeout) {
    motion_->Cancel();
    pending->failed = "超时 " + std::to_string(static_cast<int>(state.timeout)) + "s";
    return;
  }
  if (state.phase == "select") {
    if (state.job == nullptr) {
      double cx = 0, cy = 0;
      MirrorCenter(state.id, &cx, &cy);
      auto job = std::make_shared<motion::MotionJob>();
      job->kind = "select";
      job->mirror_id = mirror_visual_[state.id];
      job->has_center = true;
      job->center_x = cx;
      job->center_y = cy;
      state.job = motion_->Submit(job);
      return;
    }
    if (!state.job->failed.empty()) {
      pending->failed = "select " + state.id + ": " + state.job->failed;
      return;
    }
    if (state.job->done) {
      state.phase = "turn";
      state.job.reset();
    }
    return;
  }
  if (state.phase == "turn") {
    if (state.job == nullptr) {
      auto job = std::make_shared<motion::MotionJob>();
      job->kind = "turn";
      job->mirror_id = mirror_visual_[state.id];
      job->target_deg = state.target;
      job->tol_deg = kTurnTolCoarse;
      state.job = motion_->Submit(job);
      return;
    }
    if (!state.job->failed.empty()) {
      pending->failed = "turn " + state.id + ": " + state.job->failed;
      return;
    }
    if (state.job->done) {
      state.target =
          std::abs(state.target - state.deg2) < 1e-6 ? state.deg1 : state.deg2;
      ++state.legs;
      state.job.reset();
    }
  }
}

void PresetRunner::RotateIntervalFinish(OpPending* pending) {
  (void)pending;
  // cond held mid-sweep: stop the leg right here, keys up
  motion_->Cancel();
}

// ---- Lua bindings (runner thread) ----

namespace {

PresetRunner* RunnerOf(lua_State* L) {
  return static_cast<PresetRunner*>(
      lua_touserdata(L, lua_upvalueindex(1)));
}

void PushArgsAsJson(lua_State* L, std::vector<Json>* args) {
  const int n = lua_gettop(L);
  for (int i = 1; i <= n; ++i) {
    args->push_back(LuaToJson(L, i));
  }
}

// Blocking-op pump: mirrors preset_runner._make_blocking_wrapper. Runs on
// the runner thread inside the C function frame; aborts via lua_error.
void PumpBlockingOp(lua_State* L, const char* name,
                    std::shared_ptr<OpPending> pending) {
  PresetRunner* runner = RunnerOf(L);
  runner->SetCurrentOp(name, *pending);
  if (!pending->failed.empty()) {
    luaL_error(L, "%s: %s", name, pending->failed.c_str());
  }
  const double start = NowMonotonic();
  const bool hasDeadline = pending->timeout >= 0;
  const double deadline = hasDeadline ? start + pending->timeout : 0.0;
  const double dt = 1.0 / 30.0;
  int noteCounter = 0;
  while (!pending->done) {
    if (runner->stop_requested()) {
      luaL_error(L, "已停止");
    }
    if (std::strcmp(name, "rotate") == 0) {
      runner->RotateStep(pending.get(), dt);
    } else if (std::strcmp(name, "wait") == 0) {
      // the world advances on the frame loop; the predicate decides
    } else {
      runner->RotateIntervalStep(pending.get(), dt);
    }
    runner->SetCurrentOp(name, *pending);
    if ((++noteCounter & 7) == 0) {
      Json note = Json::Object();
      note.Set("name", Json::String(name));
      note.Set("args", Json::String(pending->args_text));
      note.Set("done", Json::Bool(pending->done));
      if (!pending->failed.empty()) note.Set("failed", Json::String(pending->failed));
      runner->log()->Event("op", note);
    }
    if (!pending->failed.empty()) {
      luaL_error(L, "%s: %s", name, pending->failed.c_str());
    }
    if (pending->cond_ref >= 0) {
      // evaluate the predicate against the fresh observation
      lua_rawgeti(L, LUA_REGISTRYINDEX, pending->cond_ref);
      PushJson(L, runner->BuildObs(L));
      if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        std::string message = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
        lua_pop(L, 1);
        luaL_error(L, "谓词执行失败: %s", message.c_str());
      }
      const bool ok = lua_toboolean(L, -1) != 0;
      lua_pop(L, 1);
      if (ok) {
        if (std::strcmp(name, "rotate_interval_until") == 0) {
          runner->RotateIntervalFinish(pending.get());
        }
        return;
      }
    }
    if (pending->exhausted) {
      luaL_error(L, "%s(%s): 搜索一圈仍未满足条件", name,
                 pending->args_text.c_str());
    }
    if (hasDeadline && NowMonotonic() >= deadline) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%.0fs", pending->timeout);
      luaL_error(L, "%s(%s): 超时 %s", name, pending->args_text.c_str(), buf);
    }
    runner->WaitNextFrame(dt);
  }
}

int LWait(lua_State* L) {
  PresetRunner* runner = RunnerOf(L);
  auto pending = std::make_shared<OpPending>();
  pending->timeout = 30.0;
  const int n = lua_gettop(L);
  if (n >= 2 && lua_istable(L, 2)) {
    lua_getfield(L, 2, "timeout");
    if (lua_isnumber(L, -1)) pending->timeout = lua_tonumber(L, -1);
    lua_pop(L, 1);
  }
  if (n >= 1 && lua_isfunction(L, 1)) {
    lua_pushvalue(L, 1);
    pending->cond_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  runner->OnWaitStart();
  PumpBlockingOp(L, "wait", pending);
  return 0;
}

int LRotate(lua_State* L) {
  std::vector<Json> args;
  PushArgsAsJson(L, &args);
  auto pending = RunnerOf(L)->RotateStart(args);
  PumpBlockingOp(L, "rotate", pending);
  return 0;
}

int LRotateInterval(lua_State* L) {
  std::vector<Json> args;
  PushArgsAsJson(L, &args);
  auto pending = RunnerOf(L)->RotateIntervalStart(args);
  if (pending->failed.empty()) {
    if (lua_gettop(L) >= 4 && lua_isfunction(L, 4)) {
      lua_pushvalue(L, 4);
      pending->cond_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    } else {
      pending->failed = "rotate_interval_until 缺少条件谓词";
    }
  }
  PumpBlockingOp(L, "rotate_interval_until", pending);
  return 0;
}

int LAngles(lua_State* L) {
  PresetRunner* runner = RunnerOf(L);
  lua_createtable(L, 0, 0);
  const vision::SceneModel* scene = runner->context()->Scene();
  if (scene == nullptr) return 1;
  for (const auto& m : scene->mirrors) {
    // authored id for this visual id
    const auto& authoredMap = runner->authored_of_mirrors();
    const auto it = authoredMap.find(m.id);
    const std::string authored = it == authoredMap.end() ? m.id : it->second;
    lua_pushstring(L, authored.c_str());
    lua_pushnumber(L, runner->MirrorAngle(authored));
    lua_settable(L, -3);
  }
  return 1;
}

int LAlive(lua_State* L) {
  PresetRunner* runner = RunnerOf(L);
  lua_createtable(L, 0, 0);
  const vision::SceneModel* scene = runner->context()->Scene();
  if (scene == nullptr) return 1;
  int index = 0;
  for (const auto& t : scene->targets) {
    if (!t.alive) continue;
    const auto& authoredMap = runner->authored_of_targets();
    const auto it = authoredMap.find(t.id);
    const std::string authored = it == authoredMap.end() ? t.id : it->second;
    lua_pushstring(L, authored.c_str());
    lua_rawseti(L, -2, ++index);
  }
  return 1;
}

int LLog(lua_State* L) {
  PresetRunner* runner = RunnerOf(L);
  const char* text = lua_tostring(L, 1);
  runner->context()->OnScriptLog(text ? text : "");
  return 0;
}

}  // namespace

void PresetRunner::Run(const std::string& code) {
  std::string baseError;
  L_ = luaL_newstate();
  if (L_ == nullptr) {
    Finish("lua state init failed");
    return;
  }
  luaL_openlibs(L_);
  const std::string baseLib = BaseLib(&baseError);
  if (baseLib.empty()) {
    Finish("脚本中止: " + baseError);
    return;
  }
  if (luaL_dostring(L_, baseLib.c_str()) != LUA_OK) {
    const char* message = lua_tostring(L_, -1);
    Finish(std::string("脚本中止: ") + (message ? message : "?"));
    return;
  }
  // register ops as closures carrying the runner pointer
  auto registerFn = [&](const char* name, lua_CFunction fn) {
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, fn, 1);
    lua_setglobal(L_, name);
  };
  registerFn("rotate", LRotate);
  registerFn("wait", LWait);
  registerFn("rotate_interval_until", LRotateInterval);
  registerFn("angles", LAngles);
  registerFn("alive", LAlive);
  registerFn("log", LLog);

  if (luaL_dostring(L_, code.c_str()) != LUA_OK) {
    const char* message = lua_tostring(L_, -1);
    if (stop_requested_) {
      FinishStopped();
      return;
    }
    Finish(std::string("脚本中止: ") + (message ? message : "?"));
    return;
  }
  if (stop_requested_) {
    FinishStopped();
    return;
  }
  Finish("");
}

void PresetRunner::FinishStopped() {
  finished_ = true;
  error_ = "";
  Json endFields = Json::Object();
  endFields.Set("reason", Json::String("stopped"));
  log_.Event("preset_end", endFields);
}

void PresetRunner::Finish(const std::string& error) {
  finished_ = true;
  error_ = error;
  if (error.empty()) {
    context_->OnScriptLog("脚本执行完成");
    Json endFields = Json::Object();
    endFields.Set("reason", Json::String("finished"));
    log_.Event("preset_end", endFields);
  } else {
    context_->OnScriptLog("脚本错误");
    Json endFields = Json::Object();
    endFields.Set("reason", Json::String(error));
    log_.Event("preset_end", endFields);
  }
}

bool PresetRunner::Start(const std::string& code, std::string* error) {
  if (thread_.joinable()) {
    *error = "preset already running";
    return false;
  }
  finished_ = false;
  error_.clear();
  stop_requested_ = false;
  thread_ = std::thread([this, code] { Run(code); });
  return true;
}

void PresetRunner::WaitNextFrame(double dt) {
  const long long current = context_->FrameNumber();
  const long long target = current + 1;
  const double deadline = NowMonotonic() + 3.0 * dt;
  while (context_->FrameNumber() < target) {
    if (NowMonotonic() >= deadline) return;
    Sleep(2);
  }
}

}  // namespace preset
