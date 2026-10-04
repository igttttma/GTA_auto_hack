#pragma once

// C++ turn controller: one-shot alignment.  A turn is ONE watched hold
// aimed directly at the target, plus — only when the coast leaves a
// residual — a computed number of timed single-step pulses.  The mirror
// coasts a FIXED angular distance past the release command (calibrated
// ~7.6 deg, measured speed-independent — see _archive/live_observation/
// calib_*); the only speed-dependent term is the link latency between
// KeyUp() and the game registering it.  The hold therefore releases when
// the predicted remaining equals coast + v*latency, scheduled on the tick
// closest to that crossing (frame period measured live), bounding release
// jitter to a quarter period.  The pulses are the deterministic finisher:
// one tap = one lattice step, so a residual of any sign costs exactly one
// pulse round — never a coast-through oscillation.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "input/held_key_input.h"
#include "tracking/tracking.h"

namespace motion {

namespace input = ::gta5::input;

constexpr double kBucket = 5.0;
constexpr double kBucketTol = kBucket / 2;
constexpr double kVWindow = 0.30;
constexpr double kVActive = 5.0;
constexpr double kVStallSeconds = 0.6;
constexpr int kTurnRetryLimit = 4;  // rounds (re-holds / pulse rounds)
constexpr double kAimSettle = 0.10;
constexpr double kDropSettle = 0.20;
constexpr double kReclickSettle = 0.15;
constexpr double kSettleTimeout = 2.5;
constexpr int kLostFrameLimit = 30;
constexpr double kDirAfterTab = 0.15;
constexpr double kMotionStart = 1.0;
constexpr double kLatencyCap = 1.2;
constexpr int kSettleWindow = 4;
constexpr double kSettleSpread = 1.0;
constexpr double kSettleMinRest = 0.45;
constexpr double kPanelK = 0.75;
constexpr double kSelectSettleSeconds = 0.1;
constexpr double kAdDeadTimeSeconds = 1.2;
constexpr double kAdDeadNetDegrees = 4.0;
constexpr int kSelectRecoveryLimit = 3;
// Coast model (calibrated, NOT learned).  A least-squares fit of
// coast = v*L + C over the 21 calibration holds (_archive/live_observation/
// calib_*) gives C ~= 7.7 deg and L ~= 0.002 s -- the coast is a fixed
// angular distance with no meaningful speed dependence, so the release only
// needs to subtract a constant.  kCoastLatencyS is the knob for the link
// latency (KeyUp -> game responds), which adds v*L of travel; it fits to ~0
// on the harness, so it starts at 0 and is raised only if a real
// speed-correlated residual shows up.  kCoastDeg is the calibrated mean
// coast; the tap finisher absorbs its ~1.7 deg shot-to-shot spread.
constexpr double kCoastDeg = 7.6;
constexpr double kCoastLatencyS = 0.0;
// Single-step pulse: a zero-duration timed tap
// moves the mirror exactly one lattice step and stops dead (the game
// finishes the current step on release; step triggering is what a longer
// hold would add).  The tap is timed by the input layer's 2 ms pump thread
// (HeldKeyInput::PulseTimed), not by the observer tick: the width lives
// near one frame period, where tick-quantized releases would be bimodal.
// kPulseGap is the key-up -> next-pulse beat (no settle between pulses in
// a round).  A residual beyond kPulseMaxPerRound steps goes back to a hold.
// Start conservatively: the first few mirrors run before the in-session EMA
// has a measured step to learn from.  The previous 76.5 ms seed consistently
// over-travelled on faster input pumps, so the initial tap must leave margin;
// later rounds still adapt toward the observed one-step width.
// Zero-duration taps are released by the input pump on its next tick. This
// avoids the startup over-travel seen with a nonzero calibrated width.
constexpr double kPulseGap = 0.08;
constexpr int kPulseMaxPerRound = 3;
// Shortest signed delta on the 0..180 ring, in (-90, 90].
double RingDelta(double target, double current);

struct MotionJob {
  std::string kind;       // "select" | "turn"
  std::string mirror_id;  // visual id
  bool has_center = false;
  double center_x = 0, center_y = 0;
  double target_deg = 0;
  double tol_deg = 3.5;
  bool done = false;
  std::string failed;  // empty = not failed

  // phase state (Python job.data)
  std::string phase;
  double entered = 0;
  // scratch / counters
  std::map<std::string, double> nums;   // settle counters, rounds, recoveries...
  bool has_v_hist = false;
  std::vector<std::pair<double, double>> v_hist;  // (observed_at, angle)
  std::vector<double> settle_window;    // per-job rest window
  bool has_center_field() const { return has_center; }
};

class PresetMotion {
 public:
  explicit PresetMotion(input::HeldKeyInput* game_input);

  void Bind(double client_origin_x, double client_origin_y,
            double panel_left, double visual_scale);
  void RearmSelectionObservation() { selection_observed_ = false; }

  // Optional key-event tap for offline dynamics fitting: (steady_clock
  // seconds, event text).  Set by the observer; unset in CLI smokes.
  using EventSink = std::function<void(double, const std::string&)>;
  void SetEventSink(EventSink sink) { event_sink_ = std::move(sink); }

  // runner-thread API
  std::shared_ptr<MotionJob> Submit(const std::shared_ptr<MotionJob>& job);
  bool Idle() const;
  void Cancel();

  // main-thread execution
  void Tick(const tracking::TrackingFrame* tracked, double frame_age);

  std::shared_ptr<MotionJob> job() const {
    return job_;
  }
  const std::string& status_text() const { return status_text_; }
  const std::string& current_selected() const { return current_selected_; }

 private:
  void Enter(MotionJob* job, const std::string& phase, double now);
  static void ClearScratch(MotionJob* job);
  double GetNum(const MotionJob& job, const std::string& key,
                double fallback = 0) const;
  void SetNum(MotionJob* job, const std::string& key, double value);

  void PointAt(MotionJob* job, double center_x, double center_y);
  void LogEvent(const std::string& text);
  void SelectTick(MotionJob* job, double now);
  void TurnTick(MotionJob* job, const tracking::TrackingFrame* tracked,
                double now, double frame_age);
  void Approach(MotionJob* job, double aim, double delta, double current,
                double now);
  void Ramp(MotionJob* job, double delta, double current, double now,
            double observed_at);
  void Spin(MotionJob* job, double delta, double current, double now,
            double observed_at);
  void Release(MotionJob* job, double delta, double current, double now,
               double observed_at, const std::string& kind);
  void Judge(MotionJob* job, double aim, double delta, double current,
             double now);
  void StartPulseRound(MotionJob* job, int sign, int pulses, double current,
                       double now);
  void PulseGap(MotionJob* job, double now);
  void JudgePulse(MotionJob* job, double aim, double delta, double current,
                  double now);
  bool NextRound(MotionJob* job, double current, double aim);
  bool AtRest(MotionJob* job, double current, double now);
  bool DeadClick(MotionJob* job, double current, double now);
  void RecoverTick(MotionJob* job, const tracking::TrackingFrame* tracked,
                   double now);
  void SelectDone(MotionJob* job);

  input::HeldKeyInput* game_input_;
  bool has_client_origin_ = false;
  double client_origin_x_ = 0, client_origin_y_ = 0;
  double panel_left_ = 0;
  double visual_scale_ = 1;
  std::string current_selected_;
  bool selection_observed_ = false;
  // Observer tick period, measured live (EMA); only used to schedule the
  // hold release on the tick closest to the predicted crossing.
  double frame_period_ = 1.0 / 30.0;
  double last_tick_at_ = 0;
  std::shared_ptr<MotionJob> job_;
  std::string status_text_;
  EventSink event_sink_;
};

}  // namespace motion
