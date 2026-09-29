#pragma once

// C++ port of the F8 preset chain: preset_runner.py (Lua host plumbing over
// a real Lua 5.3 VM replacing lupa), live_preset_host.py (ops over the live
// observation + motion) and preset_log.py (JSONL run log). The predicate
// base library is still loaded verbatim from src/lua_runtime.js so the
// simulator and the live machine cannot drift.

#include <atomic>
#include <fstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "json/json.h"
#include "preset/level_matcher.h"
#include "preset/motion.h"
#include "vision/vision.h"

struct lua_State;

namespace preset {

struct ScriptError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Observer-side context the host reads per frame.
class HostContext {
 public:
  virtual ~HostContext() = default;
  virtual const vision::SceneModel* Scene() const = 0;
  virtual const tracking::TrackingFrame* LastTracked() const = 0;
  virtual long long FrameNumber() const = 0;
  virtual void OnScriptLog(const std::string& text) = 0;
};

// State of a blocking op between frames (preset_runner.OpPending).
struct OpPending {
  bool done = false;
  std::string failed;
  bool exhausted = false;
  double timeout = -1;  // < 0: none
  std::string args_text;
  int cond_ref = -1;  // registry ref to the Lua predicate, -1 = none
  gtajson::Json log_data = gtajson::Json::Object();

  // rotate / rotate_interval_until spec state
  struct RotateState {
    std::string phase;  // select | turn
    std::string id;     // authored mirror id
    double deg = 0;
    std::shared_ptr<motion::MotionJob> job;
  };
  struct IntervalState {
    std::string phase;  // select | turn
    std::string id;
    double deg1 = 0, deg2 = 0, target = 0;
    int legs = 0;
    double elapsed = 0, timeout = 0;
    std::shared_ptr<motion::MotionJob> job;
  };
  std::shared_ptr<RotateState> rotate;
  std::shared_ptr<IntervalState> interval;
};

// JSONL run logger (preset_log.py).
class PresetLog {
 public:
  PresetLog(const std::string& path);
  void Event(const std::string& kind, const gtajson::Json& fields);
  void Frame(const gtajson::Json& record);

 private:
  std::mutex mutex_;
  std::ofstream out_;
};

// Drives one preset script against the live host (runner + host merged).
class PresetRunner {
 public:
  PresetRunner(HostContext* context, motion::PresetMotion* motion,
               const matcher::MatchResult& binding,
               std::vector<std::string> wait_lookaheads,
               const std::string& log_path);
  ~PresetRunner();

  // Compiles and spawns the runner thread. Returns false + fills error.
  bool Start(const std::string& code, std::string* error);
  // Ask the run to abort at the next frame boundary.
  void Stop() { stop_requested_ = true; }
  // Joins the runner thread once it finished; returns true on that first
  // call (and false afterwards / while still running).
  bool JoinIfFinished() {
    if (!finished_) return false;
    if (thread_.joinable()) thread_.join();
    return true;
  }
  const std::string& error() const { return error_; }

  const std::string& current_op_name() const { return current_op_name_; }
  const std::string& current_op_args() const { return current_op_args_; }
  bool stop_requested() const { return stop_requested_; }
  HostContext* context() const { return context_; }
  const std::map<std::string, std::string>& authored_of_mirrors() const {
    return mirror_authored_;
  }
  const std::map<std::string, std::string>& authored_of_targets() const {
    return target_authored_;
  }

  PresetLog* log() { return &log_; }

  // op plumbing — called by the Lua C bindings (runner thread)
  void Run(const std::string& code);
  void Finish(const std::string& error);
  void FinishStopped();
  // wait() lookahead scan (port of scan_wait_lookaheads)
  static std::vector<std::string> ScanWaitLookaheads(const std::string& code);
  std::shared_ptr<OpPending> RotateStart(const std::vector<gtajson::Json>& args);
  std::shared_ptr<OpPending> RotateIntervalStart(
      const std::vector<gtajson::Json>& args);
  void RotateStep(OpPending* pending, double dt);
  void RotateIntervalStep(OpPending* pending, double dt);
  void RotateIntervalFinish(OpPending* pending);
  void OnWaitStart();

  // host queries
  bool MirrorCenter(const std::string& authored, double* cx, double* cy) const;
  const tracking::TrackedMirror* TrackedMirror(const std::string& visual) const;
  double MirrorAngle(const std::string& authored) const;
  gtajson::Json BuildObs(lua_State* L) const;

  void WaitNextFrame(double dt);
  void SetCurrentOp(const std::string& name, const OpPending& pending);

  HostContext* context_;
  motion::PresetMotion* motion_;
  // authored <-> visual id translation (live_preset_host.py)
  std::map<std::string, std::string> mirror_visual_, mirror_authored_;
  std::map<std::string, std::string> target_visual_, target_authored_;
  std::vector<std::string> wait_lookaheads_;
  std::size_t wait_index_ = 0;
  PresetLog log_;

  std::thread thread_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> finished_{false};
  std::string error_;
  std::string current_op_name_, current_op_args_;
  lua_State* L_ = nullptr;
};

}  // namespace preset
