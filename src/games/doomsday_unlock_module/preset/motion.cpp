#include "preset/motion.h"

#include <algorithm>
#include <cmath>

namespace motion {
namespace {

constexpr WORD kVkA = input::HeldKeyInput::kKeyA;
constexpr WORD kVkD = input::HeldKeyInput::kKeyD;
constexpr WORD kVkTab = input::HeldKeyInput::kKeyTab;

double NowMonotonic() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

double RingDelta(double target, double current) {
  double d = std::fmod(target - current, 180.0);
  if (d < 0) d += 180.0;
  if (d > 90.0) {
    d -= 180.0;
  } else if (d < -90.0) {
    d += 180.0;
  }
  return d;
}

PresetMotion::PresetMotion(input::HeldKeyInput* game_input)
    : game_input_(game_input) {}

void PresetMotion::Bind(double client_origin_x, double client_origin_y,
                        double panel_left, double visual_scale) {
  has_client_origin_ = true;
  client_origin_x_ = client_origin_x;
  client_origin_y_ = client_origin_y;
  panel_left_ = panel_left;
  visual_scale_ = visual_scale;
}

std::shared_ptr<MotionJob> PresetMotion::Submit(
    const std::shared_ptr<MotionJob>& job) {
  auto current = job_;
  if (current && !current->done && current->failed.empty()) {
    // select preempts select (a wait()'s lookahead select can still be in
    // flight); every other overlap stays refused.
    if (current->kind != "select" || job->kind != "select") {
      job->failed = "MOTION_BUSY";
      job->done = true;
      return job;
    }
    current->done = true;
  }
  job_ = job;
  return job;
}

bool PresetMotion::Idle() const {
  return job_ == nullptr || job_->done || !job_->failed.empty();
}

void PresetMotion::Cancel() {
  game_input_->KeyUp();
  if (job_ && !job_->done && job_->failed.empty()) {
    job_->failed = "CANCELLED";
  }
  job_.reset();
  status_text_.clear();
}

double PresetMotion::GetNum(const MotionJob& job, const std::string& key,
                            double fallback) const {
  auto it = job.nums.find(key);
  return it == job.nums.end() ? fallback : it->second;
}

void PresetMotion::SetNum(MotionJob* job, const std::string& key, double value) {
  job->nums[key] = value;
}

void PresetMotion::ClearScratch(MotionJob* job) {
  static const char* kScratch[] = {
      "settle_window", "v_hist", "v", "v_at", "rem0", "tab_down", "pass_at",
      "net_start_angle", "pulses_left", "dropping"};
  for (const char* key : kScratch) job->nums.erase(key);
  job->has_v_hist = false;
  job->v_hist.clear();
  job->settle_window.clear();
}

void PresetMotion::Enter(MotionJob* job, const std::string& phase, double now) {
  ClearScratch(job);
  job->phase = phase;
  job->entered = now;
}

const tracking::TrackedMirror* FindMirror(
    const tracking::TrackingFrame* tracked, const std::string& mirror_id) {
  if (tracked == nullptr) return nullptr;
  for (const auto& m : tracked->mirrors) {
    if (m.mirror_id == mirror_id) return &m;
  }
  return nullptr;
}

void PresetMotion::PointAt(MotionJob* job, double center_x, double center_y) {
  const double margin = kPanelK * panel_left_ * visual_scale_;
  game_input_->MoveCursorTo(
      static_cast<LONG>(std::llround(client_origin_x_ + center_x * visual_scale_ - margin)),
      static_cast<LONG>(std::llround(client_origin_y_ + center_y * visual_scale_)));
}

void PresetMotion::LogEvent(const std::string& text) {
  if (event_sink_) event_sink_(NowMonotonic(), text);
}

void PresetMotion::Tick(const tracking::TrackingFrame* tracked, double frame_age) {
  const double now = NowMonotonic();
  if (last_tick_at_ > 0) {
    const double dt = now - last_tick_at_;
    if (dt > 0.005 && dt < 0.2) {
      frame_period_ += 0.2 * (dt - frame_period_);
    }
  }
  last_tick_at_ = now;
  if (!selection_observed_ && tracked != nullptr && !tracked->mirrors.empty()) {
    // Initialization is the ONLY read of the tracker's selected flag.
    selection_observed_ = true;
    current_selected_.clear();
    for (const auto& m : tracked->mirrors) {
      if (m.selected) {
        current_selected_ = m.mirror_id;
        break;
      }
    }
  }
  if (job_ == nullptr || job_->done || !job_->failed.empty()) return;
  if (job_->phase.empty()) {
    Enter(job_.get(), job_->kind == "select" ? "aim" : "approach", now);
  }
  if (job_->kind == "select") {
    SelectTick(job_.get(), now);
  } else {
    TurnTick(job_.get(), tracked, now, frame_age);
  }
}

void PresetMotion::SelectDone(MotionJob* job) {
  current_selected_ = job->mirror_id;
  job->done = true;
  status_text_ = "select: " + job->mirror_id + " (open loop)";
}

void PresetMotion::SelectTick(MotionJob* job, double now) {
  if (!has_client_origin_) {
    job->failed = "CLIENT_RECT_MISSING";
    return;
  }
  const std::string phase = job->phase;
  if (phase == "aim") {
    if (current_selected_ == job->mirror_id) {
      job->done = true;
      status_text_ = "select: " + job->mirror_id + " already current";
      return;
    }
    PointAt(job, job->center_x, job->center_y);
    Enter(job, "click", now);
    SetNum(job, "dropping", current_selected_.empty() ? 0.0 : 1.0);
    status_text_ = "select: cursor to " + job->mirror_id;
    return;
  }
  if (phase == "click") {
    if (now - job->entered < kAimSettle) return;
    game_input_->ClickHere(35);
    if (GetNum(*job, "dropping") == 0.0) {
      SelectDone(job);
      return;
    }
    current_selected_.clear();  // that click took the ring down
    Enter(job, "drop", now);
    status_text_ = "select: dropped via " + job->mirror_id;
    return;
  }
  if (now - job->entered < kDropSettle) return;
  game_input_->ClickHere(35);
  SelectDone(job);
}

void PresetMotion::TurnTick(MotionJob* job,
                            const tracking::TrackingFrame* tracked, double now,
                            double frame_age) {
  const std::string phase = job->phase;
  if (phase == "recover_aim" || phase == "recover_click" ||
      phase == "recover_settle") {
    RecoverTick(job, tracked, now);
    return;
  }
  const tracking::TrackedMirror* mirror = FindMirror(tracked, job->mirror_id);
  const bool readable = mirror != nullptr && mirror->misses == 0 && mirror->has_angle;
  if (!readable) {
    if (phase == "verify" || phase == "pulse_settle") {
      const double lost = GetNum(*job, "lost") + 1;
      SetNum(job, "lost", lost);
      if (lost > kLostFrameLimit) {
        job->failed = "ANGLE_OBSERVATION_LOST:" + job->mirror_id;
      }
    }
    return;
  }
  SetNum(job, "lost", 0);
  const double aim = job->target_deg;
  const double current = mirror->angle_deg;
  const double delta = RingDelta(aim, current);
  const double observed_at = now - std::max(0.0, frame_age);
  if (DeadClick(job, current, now)) return;
  if (phase == "approach") {
    Approach(job, aim, delta, current, now);
  } else if (phase == "ramp") {
    Ramp(job, delta, current, now, observed_at);
  } else if (phase == "spin") {
    Spin(job, delta, current, now, observed_at);
  } else if (phase == "pulse_gap") {
    PulseGap(job, now);
  } else if (phase == "verify") {
    Judge(job, aim, delta, current, now);
  } else {  // pulse_settle
    JudgePulse(job, aim, delta, current, now);
  }
}

void PresetMotion::Approach(MotionJob* job, double aim, double delta,
                            double current, double now) {
  if (std::abs(delta) < kBucketTol) {
    job->done = true;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "turn: %s at %.1f (asked %.0f)",
                  job->mirror_id.c_str(), current, aim);
    status_text_ = buf;
    return;
  }
  const int sign = delta > 0 ? 1 : -1;
  if (std::abs(delta) <= kBucket + kBucketTol) {
    // Within two steps of the aim: pulses only.  A hold would coast its
    // several degrees straight through a residual this small.
    const int n = static_cast<int>(std::rint(delta / kBucket));
    if (n == 0) {
      job->done = true;
      char buf[96];
      std::snprintf(buf, sizeof(buf), "turn: %s at %.1f (asked %.0f)",
                    job->mirror_id.c_str(), current, aim);
      status_text_ = buf;
      return;
    }
    if (!NextRound(job, current, aim)) return;
    StartPulseRound(job, n > 0 ? 1 : -1, std::abs(n), current, now);
    return;
  }
  if (GetNum(*job, "tab_down") == 0.0) {
    game_input_->SetTurbo(kVkTab);
    LogEvent("tab_down");
    SetNum(job, "tab_down", 1.0);
    SetNum(job, "pass_at", now);
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "turn: %s -> %.0f (%s) delta=%+.1f coast=%.1fdeg",
                  job->mirror_id.c_str(), aim, sign > 0 ? "A" : "D", delta,
                  kCoastDeg);
    status_text_ = buf;
    return;
  }
  if (now - GetNum(*job, "pass_at") < kDirAfterTab) return;
  game_input_->KeyDown(sign > 0 ? kVkA : kVkD);
  LogEvent(sign > 0 ? "dir_down A" : "dir_down D");
  const double rem0 = std::abs(delta);
  Enter(job, "ramp", now);
  SetNum(job, "sign", sign);
  SetNum(job, "keys_at", now);
  SetNum(job, "rem0", rem0);
}

void PresetMotion::Ramp(MotionJob* job, double delta, double current,
                        double now, double observed_at) {
  if (GetNum(*job, "rem0") - std::abs(delta) >= kMotionStart) {
    const double sign = GetNum(*job, "sign");
    const double keysAt = GetNum(*job, "keys_at");
    Enter(job, "spin", now);
    SetNum(job, "sign", sign);
    SetNum(job, "keys_at", keysAt);
    SetNum(job, "v", 0.0);
    return;
  }
  if (now - GetNum(*job, "keys_at") > kLatencyCap) {
    Release(job, delta, current, now, observed_at, "dead");
  }
}

void PresetMotion::Spin(MotionJob* job, double delta, double current,
                        double now, double observed_at) {
  if (!job->has_v_hist) {
    job->has_v_hist = true;
    job->v_hist.clear();
  }
  job->v_hist.push_back({observed_at, current});
  while (job->v_hist.size() > 1 && observed_at - job->v_hist.front().first > kVWindow) {
    job->v_hist.erase(job->v_hist.begin());
  }
  if (job->v_hist.size() >= 2) {
    const auto [t0, a0] = job->v_hist.front();
    const auto [t1, a1] = job->v_hist.back();
    if (t1 > t0) SetNum(job, "v", RingDelta(a1, a0) / (t1 - t0));
  }
  const double sign = GetNum(*job, "sign");
  const double vAlong = GetNum(*job, "v") * sign;
  if (vAlong > kVActive) {
    SetNum(job, "v_at", now);
  } else if (job->nums.count("v_at") &&
             now - GetNum(*job, "v_at") > kVStallSeconds) {
    Release(job, delta, current, now, observed_at, "stall");
    return;
  }
  const double deltaSign = delta > 0 ? 1 : (delta < 0 ? -1 : 0);
  if (deltaSign != sign) {
    Release(job, delta, current, now, observed_at, "crossed");
    return;
  }
  if (vAlong <= 0) return;
  // The hold aims at the target directly; the deterministic single-step
  // pulses absorb whatever residual the coast leaves (undershoot costs a
  // forward pulse, overshoot a reverse one -- one round either way, never
  // a coast-through oscillation).  Release on the tick closest to the
  // predicted crossing: at 30 Hz the release jitter is a quarter period,
  // not a full one.  The coast is a fixed angular distance (calibrated,
  // speed-independent) plus an optional link-latency term v*L for the
  // key-up-to-response delay; nothing is learned at runtime.
  const double predicted_stop_travel = kCoastDeg + vAlong * kCoastLatencyS;
  const double t_cross =
      observed_at + (std::abs(delta) - predicted_stop_travel) / vAlong;
  if (t_cross <= now + 0.5 * frame_period_) {
    Release(job, delta, current, now, observed_at, "margin");
  }
}

void PresetMotion::Release(MotionJob* job, double delta, double current,
                           double now, double observed_at,
                           const std::string& kind) {
  (void)delta;
  (void)current;
  (void)observed_at;
  game_input_->KeyUp();
  LogEvent("key_up " + kind);
  Enter(job, "verify", now);
}

void PresetMotion::Judge(MotionJob* job, double aim, double delta,
                         double current, double now) {
  if (!AtRest(job, current, now)) {
    if (now - job->entered > kSettleTimeout) {
      job->failed = "SETTLE_TIMEOUT:" + job->mirror_id;
    }
    return;
  }
  const int n = static_cast<int>(std::rint(delta / kBucket));
  if (std::abs(delta) < kBucketTol || n == 0) {
    // n == 0: exactly between two lattice steps -- the mirror is already at
    // the game's own nearest answer.
    job->done = true;
    char buf[112];
    std::snprintf(buf, sizeof(buf),
                  "turn: %s at %.1f (asked %.0f, %d rounds)",
                  job->mirror_id.c_str(), current, aim,
                  static_cast<int>(GetNum(*job, "rounds")));
    status_text_ = buf;
    return;
  }
  if (std::abs(delta) > (kPulseMaxPerRound + 0.5) * kBucket) {
    // Out of pulse range: back to a hold from wherever it sits.
    if (!NextRound(job, current, aim)) return;
    Enter(job, "approach", now);
    return;
  }
  if (!NextRound(job, current, aim)) return;
  StartPulseRound(job, n > 0 ? 1 : -1, std::abs(n), current, now);
}

bool PresetMotion::NextRound(MotionJob* job, double current, double aim) {
  const int rounds = static_cast<int>(GetNum(*job, "rounds")) + 1;
  SetNum(job, "rounds", rounds);
  if (rounds > kTurnRetryLimit) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "TURN_NOT_CONVERGING:%s:at %.1f want %.0f",
                  job->mirror_id.c_str(), current, aim);
    job->failed = buf;
    return false;
  }
  return true;
}

void PresetMotion::StartPulseRound(MotionJob* job, int sign, int pulses,
                                   double current, double now) {
  game_input_->SetTurbo(0);
  Enter(job, "pulse_gap", now);
  SetNum(job, "pulse_sign", sign);
  SetNum(job, "pulses_left", pulses);
  SetNum(job, "round_start_angle", current);
  SetNum(job, "round_pulses", pulses);
  game_input_->PulseTimed(sign > 0 ? kVkA : kVkD, pulse_width_);
  {
    char ev[64];
    std::snprintf(ev, sizeof(ev), "pulse %s %.3f", sign > 0 ? "A" : "D",
                  pulse_width_);
    LogEvent(ev);
  }
  char buf[128];
  std::snprintf(buf, sizeof(buf), "turn: %s pulse %s x%d (round %d, w=%.3fs)",
                job->mirror_id.c_str(), sign > 0 ? "A" : "D", pulses,
                static_cast<int>(GetNum(*job, "rounds")), pulse_width_);
  status_text_ = buf;
}

void PresetMotion::PulseGap(MotionJob* job, double now) {
  // The timed release comes from the input layer's pump thread, so the gap
  // only paces the next pulse; no settle between pulses in a round.
  if (now - job->entered < pulse_width_ + kPulseGap) return;
  const int left = static_cast<int>(GetNum(*job, "pulses_left"));
  if (left <= 1) {
    Enter(job, "pulse_settle", now);
    return;
  }
  const int sign = static_cast<int>(GetNum(*job, "pulse_sign"));
  Enter(job, "pulse_gap", now);
  SetNum(job, "pulses_left", left - 1);
  game_input_->PulseTimed(sign > 0 ? kVkA : kVkD, pulse_width_);
  {
    char ev[64];
    std::snprintf(ev, sizeof(ev), "pulse %s %.3f", sign > 0 ? "A" : "D",
                  pulse_width_);
    LogEvent(ev);
  }
}

void PresetMotion::JudgePulse(MotionJob* job, double aim, double delta,
                              double current, double now) {
  if (!AtRest(job, current, now)) {
    if (now - job->entered > kSettleTimeout) {
      job->failed = "SETTLE_TIMEOUT:" + job->mirror_id;
    }
    return;
  }
  // Pulse-width calibration: the round's measured per-pulse displacement
  // steers the width toward exactly one lattice step (EMA, in-session only --
  // it restarts from kPulseWidthSeed each launch, nothing is persisted).
  // Below 1 deg the pulse did not really move (dropped pulse, misread) and
  // says nothing; above ~2 steps the reading is too coarse to steer on.
  const double fired = GetNum(*job, "round_pulses");
  if (fired >= 1 && job->nums.count("round_start_angle")) {
    const double per =
        RingDelta(current, GetNum(*job, "round_start_angle")) *
        GetNum(*job, "pulse_sign") / fired;
    if (per >= 1.0 && per <= 11.0) {
      const double sample = pulse_width_ * (kBucket / per);
      pulse_width_ = std::min(
          kPulseClampMax,
          std::max(kPulseClampMin,
                   pulse_width_ + kPulseLearn * (sample - pulse_width_)));
    }
  }
  const int n = static_cast<int>(std::rint(delta / kBucket));
  if (std::abs(delta) < kBucketTol || n == 0) {
    job->done = true;
    char buf[112];
    std::snprintf(buf, sizeof(buf),
                  "turn: %s at %.1f (asked %.0f, %d rounds)",
                  job->mirror_id.c_str(), current, aim,
                  static_cast<int>(GetNum(*job, "rounds")));
    status_text_ = buf;
    return;
  }
  // Anti-oscillation guard: the round that just settled crossed the target --
  // the residual now has the opposite sign to the tap direction, so the tap
  // over-travelled (typically a second lattice step) and landed ~a full step
  // past the aim. Tapping straight back at the same/EMA width is exactly the
  // +-5 deg limit cycle; instead hard-cut the width and fire ONE corrective
  // tap, which now under-travels into the done band. Bounded by NextRound's
  // kTurnRetryLimit, so a pathological width still fails loudly, not forever.
  const double lastSign = GetNum(*job, "pulse_sign");
  if (lastSign != 0.0 && delta * lastSign < 0.0) {
    const int overshoots =
        static_cast<int>(GetNum(*job, "overshoot_flips")) + 1;
    SetNum(job, "overshoot_flips", overshoots);
    pulse_width_ = std::min(
        kPulseClampMax, std::max(kPulseClampMin, pulse_width_ * kOvershootWidthCut));
    LogEvent("overshoot x" + std::to_string(overshoots) + " width->" +
             std::to_string(pulse_width_));
    if (!NextRound(job, current, aim)) return;
    StartPulseRound(job, delta > 0 ? 1 : -1, 1, current, now);
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "turn: %s overshoot #%d -> corrective tap %s (w=%.3fs)",
                  job->mirror_id.c_str(), overshoots, delta > 0 ? "A" : "D",
                  pulse_width_);
    status_text_ = buf;
    return;
  }
  SetNum(job, "overshoot_flips", 0);
  if (std::abs(delta) > (kPulseMaxPerRound + 0.5) * kBucket) {
    if (!NextRound(job, current, aim)) return;
    Enter(job, "approach", now);
    return;
  }
  if (!NextRound(job, current, aim)) return;
  StartPulseRound(job, n > 0 ? 1 : -1, std::abs(n), current, now);
}

bool PresetMotion::AtRest(MotionJob* job, double current, double now) {
  auto& window = job->settle_window;
  window.push_back(current);
  if (window.size() > static_cast<std::size_t>(kSettleWindow)) {
    window.erase(window.begin());
  }
  // Ring-aware spread: raw max-min explodes at the 0/180 wrap (readings
  // flicker between 179.5 and 0.0 while the mirror sits still in the 180
  // bucket), which made wrap-adjacent aims SETTLE_TIMEOUT.
  const double ref = window.front();
  double lo = 0.0, hi = 0.0;
  bool first = true;
  for (const double v : window) {
    const double d = RingDelta(v, ref);  // (-90, 90]
    if (first) {
      lo = hi = d;
      first = false;
    } else {
      lo = std::min(lo, d);
      hi = std::max(hi, d);
    }
  }
  return window.size() >= static_cast<std::size_t>(kSettleWindow) &&
         hi - lo < kSettleSpread && now - job->entered > kSettleMinRest;
}

bool PresetMotion::DeadClick(MotionJob* job, double current, double now) {
  if (job->phase != "ramp" && job->phase != "spin") return false;
  if (!job->nums.count("net_start_angle")) {
    SetNum(job, "net_start_angle", current);
  }
  const double netStart = GetNum(*job, "net_start_angle");
  const double m0 = std::fmod(current - netStart + 90.0, 180.0);
  const double net =
      std::abs((m0 < 0 ? m0 + 180.0 : m0) - 90.0);  // Python % semantics
  if (now - GetNum(*job, "keys_at") < kAdDeadTimeSeconds ||
      net >= kAdDeadNetDegrees) {
    return false;
  }
  if (static_cast<int>(GetNum(*job, "recoveries")) >= kSelectRecoveryLimit) {
    job->failed = "SELECT_UNRESPONSIVE:" + job->mirror_id;
    return true;
  }
  SetNum(job, "recoveries", GetNum(*job, "recoveries") + 1);
  game_input_->KeyUp();
  LogEvent("key_up deadclick");
  Enter(job, "recover_aim", now);
  status_text_ = "turn: " + job->mirror_id + " no A/D response, re-clicking";
  return true;
}

void PresetMotion::RecoverTick(MotionJob* job,
                               const tracking::TrackingFrame* tracked,
                               double now) {
  const std::string phase = job->phase;
  if (!has_client_origin_) {
    job->failed = "CLIENT_RECT_MISSING";
    return;
  }
  if (phase == "recover_aim") {
    const tracking::TrackedMirror* mirror = FindMirror(tracked, job->mirror_id);
    if (mirror != nullptr) {
      job->center_x = mirror->cx;
      job->center_y = mirror->cy;
      job->has_center = true;
    }
    if (!job->has_center) {
      return;  // no aim point yet; retry on the next frame
    }
    const double cx = job->center_x, cy = job->center_y;
    PointAt(job, cx, cy);
    Enter(job, "recover_click", now);
    return;
  }
  if (phase == "recover_click") {
    if (now - job->entered < kReclickSettle) return;
    game_input_->ClickHere(35);
    current_selected_ = job->mirror_id;
    Enter(job, "recover_settle", now);
    status_text_ = "turn: re-clicked " + job->mirror_id;
    return;
  }
  if (now - job->entered < kSelectSettleSeconds) return;
  Enter(job, "approach", now);
}

}  // namespace motion
