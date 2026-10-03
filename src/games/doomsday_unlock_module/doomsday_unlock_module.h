#pragma once

// Doomsday unlock module (ported from gtascript3's F6 one-key flow).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <functional>

#include "capture/game_window.h"
#include "imgproc/imgproc.h"
#include "input/held_key_input.h"
#include "overlay/overlay.h"
#include "preset/level_matcher.h"
#include "preset/motion.h"
#include "preset/preset_host.h"
#include "tracking/tracking.h"
#include "vision/vision.h"

namespace gta5::games::doomsday_unlock {

namespace input = ::gta5::input;

using gtajson::Json;

struct FrameScheduler {
  explicit FrameScheduler(double hz);
  void Reset();
  bool Due();
  double period_s;
  LARGE_INTEGER next_due{};
  LARGE_INTEGER frequency{};
};

class ObserverApp : public preset::HostContext {
 public:
  static ObserverApp& Instance();

  bool DetectInGame(const gta5::capture::GameFrame& frame);
  void ResetInGameCache();
  bool RunSession(const std::function<bool()>& stopRequested,
                  const std::function<bool()>& overlayEnabled,
                  const std::function<void(const std::wstring&)>& status);
  void SetOverlayWindow(HWND hwnd);
  void ClearOverlay();
  LRESULT OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

  // preset::HostContext (read by the Lua preset runner)
  const vision::SceneModel* Scene() const override {
    return active_ ? &scene_ : nullptr;
  }
  const tracking::TrackingFrame* LastTracked() const override {
    return last_tracked_.get();
  }
  long long FrameNumber() const override { return frame_number_; }
  void OnScriptLog(const std::string& text) override { (void)text; }

 private:
  ObserverApp() = default;
  ~ObserverApp();
  void StartAttack();
  void CancelAttack(const std::string& reason, bool report);
  void FailAttack(const std::string& reason);
  void PresetFinished();
  void ApplyBinding(const matcher::MatchResult& match);
  void WritePresetFrame(const tracking::TrackingFrame& tracked);
  bool Start();
  void Stop();
  void Report(const char* localization_key);
  void ObserveFrame();
  void RefreshTargetAlive(const imgproc::Mat8& frame);
  bool ConfigureRegion(const RECT& client_rect);
  void StopGrabber();
  void DroppedCaptureFrame();

  vision::BoardVision vision_;
  tracking::FixedMirrorAngleTracker* tracker_ = nullptr;
  std::unique_ptr<tracking::FixedMirrorAngleTracker> tracker_storage_;
  vision::SceneModel scene_;
  FrameScheduler scheduler_{30.0};

  std::atomic<bool> active_{false};
  bool completed_ = false;
  std::function<bool()> stop_requested_;
  std::function<bool()> overlay_enabled_;
  std::function<void(const std::wstring&)> status_;
  bool grabber_thread_alive_ = false;
  bool grabber_running_ = false;  // grabber-loop switch, independent of active_
  bool auto_attack_pending_ = false;  // F6 one-key: attack once first frame lands
  std::thread grabber_;
  std::mutex grabber_mutex_;
  std::vector<std::uint32_t> grabber_frame_;  // newest BGRA region crop
  int grabber_w_ = 0, grabber_h_ = 0;
  double grabber_timestamp_ = 0.0;  // seconds, steady clock
  double taken_timestamp_ = 0.0;
  double grabber_bitblt_ms_ = 0.0;
  std::string grabber_error_;
  RECT grabber_client_{};

  double processing_scale_ = 1.0;
  int processing_w_ = 0, processing_h_ = 0;
  imgproc::Mat8 processing_frame_;  // persistent full processing frame
  // region mapping (see live_overlay.RegionFrameCapture.configure)
  bool region_active_ = false;
  int region_px0_ = 0, region_py0_ = 0, region_pw_ = 0, region_ph_ = 0;
  RECT region_screen_{};

  int capture_missing_frames_ = 0;
  int ingame_missing_frames_ = 0;
  // full-frame Ingame() gate is expensive (HSV + connected components); the
  // exit check only needs to fire every N frames — 4 misses * 15 frames is
  // still ~2s of latency at 30Hz, same as the original per-frame tolerance
  static constexpr int kIngameCheckPeriod = 15;
  static constexpr int kIngameFastMissTolerance = 4;
  long long frame_number_ = 0;
  double started_at_ = 0;
  double last_frame_pc_ = 0;
  double fps_ema_ = 0;
  std::map<std::string, std::vector<int>> alive_recent_;
  std::set<std::string> lost_dumped_;  // mirrors whose lost-frame dump was written
  static constexpr int kLostFrameDumpAt = 8;  // dump frame at this miss count

  // Ghost-frame dump (2026-09-26, user rule "一剧烈跳直接转储"): ANY mirror
  // whose angle jumps more than kGhostDumpJumpDeg between two of its
  // measurements is not physical — the whole processing frame is saved as
  // ghost_frame_*.bmp immediately for offline replay.
  std::map<std::string, double> last_sel_angle_;
  int ghost_dumps_ = 0;
  static constexpr int kGhostDumpCap = 12;
  static constexpr double kGhostDumpJumpDeg = 25.0;

  // preset attack chain (level binding + Lua host + motion layer), fired
  // automatically once the first observed frame is tracked
  std::unique_ptr<input::HeldKeyInput> game_input_;
  std::unique_ptr<motion::PresetMotion> preset_motion_;
  std::unique_ptr<preset::PresetRunner> preset_runner_;
  std::unique_ptr<tracking::TrackingFrame> last_tracked_;
  std::string attack_state_ = "IDLE";  // IDLE | PRESET_RUN
  bool has_binding_ = false;
  matcher::MatchResult level_binding_;
  double frame_taken_at_ = 0;

  HWND overlay_hwnd_ = nullptr;
  std::mutex overlay_mutex_;
  std::vector<overlay::Label> overlay_labels_;
  std::string overlay_hud_;
  bool overlay_cleared_ = true;    // no pending labels; skip repeated clears
  bool overlay_positioned_ = false;  // SetWindowPos already matches the client
  RECT overlay_client_{};            // client rect the overlay was positioned for

  bool have_pending_ingame_ = false;
  gta5::capture::GameFrame pending_frame_;
  int pending_ingame_rect_[4] = {0, 0, 0, 0};
  vision::IngameAnchors ingame_anchors_;  // cheap per-frame liveness anchors
  unsigned long long cache_window_generation_ = 0;
  int cache_width_ = 0;
  int cache_height_ = 0;
};

bool DetectInGame(const gta5::capture::GameFrame& frame);
void ResetInGameCache();
bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<bool()>& overlayEnabled,
                const std::function<void(const std::wstring&)>& status);
void SetOverlayWindow(HWND hwnd);
void ClearOverlay();
LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

}  // namespace gta5::games::doomsday_unlock
