#include "doomsday_unlock_module.h"

#include "common/processing.h"
#include "app/localization.h"
#include "doomsday_embedded.h"

#include <cmath>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace gta5::games::doomsday_unlock {
namespace {

using gta5::capture::CaptureGameFrameStatus;
using gta5::capture::CaptureStatus;
using gta5::capture::FindGameWindow;
using gta5::capture::GameFrame;
using gta5::capture::GetGameClientRect;

constexpr int kAliveMissLimit = 4;
constexpr int kAliveMissWindow = 6;

double NowSeconds() {
  static LARGE_INTEGER freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f;
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
}

std::wstring WidenAscii(const std::string& text) {
  return std::wstring(text.begin(), text.end());
}

std::wstring WidenUtf8(const std::string& text) {
  if (text.empty()) return {};
  const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
  if (len <= 0) return WidenAscii(text);
  std::wstring out(static_cast<std::size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), len);
  if (!out.empty() && out.back() == L'\0') out.pop_back();
  return out;
}

imgproc::Mat8 FrameToBgr(const GameFrame& frame) {
  imgproc::Mat8 bgr(frame.height, frame.width, 3);
  for (int y = 0; y < frame.height; ++y) {
    const std::uint32_t* src =
        frame.bgra.data() + static_cast<std::size_t>(y) * frame.width;
    std::uint8_t* dst = bgr.Ptr(y);
    for (int x = 0; x < frame.width; ++x) {
      dst[3 * x + 0] = static_cast<std::uint8_t>(src[x] & 0xFF);
      dst[3 * x + 1] = static_cast<std::uint8_t>((src[x] >> 8) & 0xFF);
      dst[3 * x + 2] = static_cast<std::uint8_t>((src[x] >> 16) & 0xFF);
    }
  }
  return bgr;
}

}  // namespace

FrameScheduler::FrameScheduler(double hz)
    : period_s(1.0 / hz) {
  QueryPerformanceFrequency(&frequency);
}

void FrameScheduler::Reset() {
  QueryPerformanceCounter(&next_due);
  next_due.QuadPart += static_cast<LONGLONG>(period_s * frequency.QuadPart);
}

bool FrameScheduler::Due() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (now.QuadPart < next_due.QuadPart) return false;
  // consume one period (Python scheduler semantics)
  next_due.QuadPart += static_cast<LONGLONG>(period_s * frequency.QuadPart);
  if (next_due.QuadPart < now.QuadPart) {
    next_due.QuadPart = now.QuadPart +
        static_cast<LONGLONG>(period_s * frequency.QuadPart);
  }
  return true;
}

ObserverApp& ObserverApp::Instance() {
  static ObserverApp app;
  return app;
}

void ObserverApp::Report(const char* localization_key) {
  if (status_) status_(gta5::app::l10n::Text(localization_key));
}

void ObserverApp::SetOverlayWindow(HWND hwnd) {
  overlay_hwnd_ = hwnd;
  overlay_positioned_ = false;
  ClearOverlay();
}

void ObserverApp::ClearOverlay() {
  {
    std::lock_guard<std::mutex> lock(overlay_mutex_);
    overlay_labels_.clear();
    overlay_hud_.clear();
  }
  overlay_positioned_ = false;
  if (overlay_cleared_) return;  // nothing on screen to clear
  overlay_cleared_ = true;
  if (overlay_hwnd_) {
    InvalidateRect(overlay_hwnd_, nullptr, TRUE);
    ShowWindow(overlay_hwnd_, SW_HIDE);
  }
}

LRESULT ObserverApp::OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      std::vector<overlay::Label> labels;
      std::string hud;
      {
        std::lock_guard<std::mutex> lock(overlay_mutex_);
        labels = overlay_labels_;
        hud = overlay_hud_;
      }
      PAINTSTRUCT ps{};
      HDC dc = BeginPaint(hwnd, &ps);
      RECT rect{};
      GetClientRect(hwnd, &rect);
      HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
      FillRect(dc, &rect, black);
      DeleteObject(black);
      const int height = rect.bottom - rect.top;
      const double scale = height > 0 ? height / geom::kRefHeight : 1.0;
      SetBkMode(dc, TRANSPARENT);
      const int fontPx = std::max(1, static_cast<int>(std::lround(28 * scale)));
      HFONT font = CreateFontW(fontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               FIXED_PITCH | FF_MODERN, L"Consolas");
      HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font));
      const int halfW = static_cast<int>(std::lround(160 * scale));
      const int lineHeight = static_cast<int>(std::lround(34 * scale));
      for (const overlay::Label& label : labels) {
        SetTextColor(dc, label.color);
        const int cx = static_cast<int>(std::lround(label.x));
        const int cy = static_cast<int>(std::lround(label.y));
        const int r = static_cast<int>(std::lround(label.radius));
        const int anchorX =
            label.text_at_pos ? static_cast<int>(std::lround(label.text_x)) : cx;
        const int anchorY =
            label.text_at_pos ? static_cast<int>(std::lround(label.text_y))
                              : cy - r - static_cast<int>(std::lround(36 * scale));
        RECT textRect = {anchorX - halfW, anchorY, anchorX + halfW,
                         anchorY + lineHeight};
        const std::wstring text = WidenUtf8(label.text);
        DrawTextW(dc, text.c_str(), -1, &textRect,
                  label.text_at_pos
                      ? (DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOCLIP)
                      : (DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP));
      }
      if (!hud.empty()) {
        SetTextColor(dc, RGB(215, 249, 241));
        const std::wstring text = WidenUtf8(hud);
        RECT hudRect = {8, 8, 8 + static_cast<int>(std::lround(300 * scale)),
                        8 + static_cast<int>(std::lround(32 * scale))};
        DrawTextW(dc, text.c_str(), -1, &hudRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
      }
      SelectObject(dc, oldFont);
      DeleteObject(font);
      EndPaint(hwnd, &ps);
      return 0;
    }
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

ObserverApp::~ObserverApp() {
  // a joinable std::thread destroyed -> std::terminate; always wind down
  Stop();
}

void ObserverApp::StopGrabber() {
  // only stops the grabber thread; never touches active_ — the full-frame
  // fallback path calls this while observation must keep running
  grabber_running_ = false;
  if (grabber_.joinable()) {
    grabber_.join();
  }
  region_active_ = false;
  grabber_error_.clear();
}

bool ObserverApp::Start() {
  if (active_) return true;
  Report("status.doomsday.locating");
  if (!FindGameWindow()) {
    Report("status.doomsday.failed");
    return false;
  }

  RECT client{};
  if (!GetGameClientRect(client)) {
    Report("status.doomsday.capture_failed");
    return false;
  }
  GameFrame frame;
  if (have_pending_ingame_) {
    frame = std::move(pending_frame_);
    have_pending_ingame_ = false;
  } else {
    CaptureGameFrameStatus(frame, nullptr);
  }
  if (frame.bgra.empty()) {
    Report("status.doomsday.capture_failed");
    return false;
  }
  // capture module owns scaling (>1080 -> 1080): this IS the processing frame
  processing_w_ = frame.width;
  processing_h_ = frame.height;
  processing_scale_ = geom::ClientCaptureScale(frame.clientHeight);
  processing_frame_ = FrameToBgr(frame);

  const vision::GateResult ingame =
      vision_.Ingame(processing_frame_, &ingame_anchors_);
  if (ingame.status != "YES") {
    Report("status.doomsday.failed");
    return false;
  }

  // The scene can still be mid-animation right after the ingame gate passes
  // (entities not settled -> DetectLiveScene or MatchLevel fails on a frame
  // that will look fine moments later): re-capture and retry the whole
  // post-ingame init a few times before giving up.
  constexpr int kInitAttempts = 4;
  constexpr DWORD kInitRetryDelayMs = 200;
  bool scene_ready = false;
  for (int attempt = 1; attempt <= kInitAttempts && !scene_ready; ++attempt) {
    if (attempt > 1) {
      Sleep(kInitRetryDelayMs);
      GameFrame retry_frame;
      CaptureGameFrameStatus(retry_frame, nullptr);
      if (retry_frame.bgra.empty()) {
        Report("status.doomsday.capture_failed");
        return false;
      }
      if (retry_frame.width != processing_w_ ||
          retry_frame.height != processing_h_) {
        ingame_anchors_ = vision::IngameAnchors{};
        processing_w_ = retry_frame.width;
        processing_h_ = retry_frame.height;
        processing_scale_ = geom::ClientCaptureScale(retry_frame.clientHeight);
      }
      processing_frame_ = FrameToBgr(retry_frame);
      if (vision_.Ingame(processing_frame_, &ingame_anchors_).status != "YES") {
        Report("status.doomsday.failed");
        return false;
      }
    }

    vision::SceneModel model;
    const vision::GateResult entities =
        vision_.DetectLiveScene(processing_frame_, ingame.rect, &model);
    if (entities.status != "YES") {
      continue;
    }

    // Python binds authored ids BEFORE anything downstream sees the model
    // (tracker, overlay, CSV, artifacts): sequential vision ids shift with
    // reading order and whenever a mount is deduped, while preset scripts and
    // the simulator reference the authored ids. A failed match here is
    // treated as a not-yet-ready scene and retried.
    std::vector<std::pair<double, double>> mirrors, targets;
    for (const auto& m : model.mirrors) mirrors.push_back({m.cx, m.cy});
    for (const auto& t : model.targets) targets.push_back({t.cx, t.cy});
    matcher::MatchResult match;
    try {
      match = matcher::MatchLevel(mirrors, targets, model.playfield);
    } catch (const std::exception& exc) {
      (void)exc;
      continue;
    }
    if (!match.missing_mirrors.empty()) {
      // matched a level but some required mirrors were not detected (a
      // transient miss, e.g. the top-left mirror behind the game UI). The
      // level preset needs every one of them, so re-detect instead of
      // starting an attack that would abort on an unknown mirror id.
      continue;
    }
    scene_ = model;
    ApplyBinding(match);
    scene_ready = true;
  }
  if (!scene_ready) {
    Report("status.doomsday.failed");
    return false;
  }

  tracker_storage_ = std::make_unique<tracking::FixedMirrorAngleTracker>(
      scene_.mirrors, scene_.processing_w, scene_.processing_h);
  tracker_ = tracker_storage_.get();

  capture_missing_frames_ = 0;
  ingame_missing_frames_ = 0;
  frame_number_ = 0;
  started_at_ = NowSeconds();
  scheduler_.Reset();
  {
    // Release build keeps the motion event sink disabled; no local run logs.
    const double steady0 = std::chrono::duration<double>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    const double qpc0 = NowSeconds();
    preset_motion_->SetEventSink(nullptr);
  }

  if (!ConfigureRegion(client)) {
    Report("status.doomsday.failed");
    tracker_storage_.reset();
    tracker_ = nullptr;
    return false;
  }
  active_ = true;
  grabber_running_ = true;
  grabber_thread_alive_ = true;
  grabber_ = std::thread([this] {
    const int periodMs = 8;  // ~120 Hz loop like the Python grabber
    while (grabber_running_ && grabber_thread_alive_) {
      const double started = NowSeconds();
      RECT client{};
      if (!GetGameClientRect(client) || !EqualRect(&client, &grabber_client_)) {
        std::lock_guard<std::mutex> lock(grabber_mutex_);
        grabber_error_ = "CLIENT_RECT_CHANGED";
        grabber_thread_alive_ = false;
        return;
      }
      GameFrame crop;
      const gta5::capture::CaptureStatus status =
          CaptureGameFrameStatus(crop, &region_screen_);
      if (status == gta5::capture::CaptureStatus::Error) {
        std::lock_guard<std::mutex> lock(grabber_mutex_);
        grabber_error_ = "BITBLT_FAILED";
        grabber_thread_alive_ = false;
        return;
      }
      const double finished = NowSeconds();
      {
        std::lock_guard<std::mutex> lock(grabber_mutex_);
        grabber_frame_ = std::move(crop.bgra);
        grabber_w_ = crop.width;
        grabber_h_ = crop.height;
        grabber_timestamp_ = finished;
        grabber_bitblt_ms_ = (finished - started) * 1000.0;
      }
      const double remaining = periodMs - (NowSeconds() - started) * 1000.0;
      if (remaining > 0) Sleep(static_cast<DWORD>(remaining));
    }
  });

  Report("status.doomsday.running");
  // F6 one-key flow: the attack (level match + Lua preset) fires as soon as
  // the first observed frame has been tracked; terminal state -> Stop()
  auto_attack_pending_ = true;
  return true;
}

bool ObserverApp::ConfigureRegion(const RECT& client_rect) {
  StopGrabber();
  int bounds[4];
  tracker_->ObservedBounds(bounds);
  int x0 = bounds[0], y0 = bounds[1];
  int x1 = bounds[0] + bounds[2], y1 = bounds[1] + bounds[3];
  for (const auto& target : scene_.targets) {
    const double tx = target.rect[0], ty = target.rect[1];
    const double tw = target.rect[2], th = target.rect[3];
    x0 = static_cast<int>(std::min(static_cast<double>(x0), tx));
    y0 = static_cast<int>(std::min(static_cast<double>(y0), ty));
    x1 = static_cast<int>(std::max(static_cast<double>(x1), tx + tw));
    y1 = static_cast<int>(std::max(static_cast<double>(y1), ty + th));
  }
  const double margin = 8.0;
  x0 = static_cast<int>(std::floor(x0 - margin));
  y0 = static_cast<int>(std::floor(y0 - margin));
  x1 = static_cast<int>(std::ceil(x1 + margin));
  y1 = static_cast<int>(std::ceil(y1 + margin));

  // RegionFrameCapture.configure mapping: drive from the client side so the
  // crop lands on integer source pixels.
  int px0 = std::max(0, std::min(x0, processing_w_ - 1));
  int py0 = std::max(0, std::min(y0, processing_h_ - 1));
  int pw = std::max(1, std::min(x1 - x0, processing_w_ - px0));
  int ph = std::max(1, std::min(y1 - y0, processing_h_ - py0));
  const int clientW = client_rect.right - client_rect.left;
  const int clientH = client_rect.bottom - client_rect.top;
  int left = static_cast<int>(std::rint(px0 / processing_scale_));
  int top = static_cast<int>(std::rint(py0 / processing_scale_));
  int width = std::min(static_cast<int>(std::rint(pw / processing_scale_)), clientW - left);
  int height = std::min(static_cast<int>(std::rint(ph / processing_scale_)), clientH - top);
  if (left < 0 || top < 0 || width <= 0 || height <= 0) return false;
  region_px0_ = static_cast<int>(std::rint(left * processing_scale_));
  region_py0_ = static_cast<int>(std::rint(top * processing_scale_));
  region_pw_ = std::min(static_cast<int>(std::rint(width * processing_scale_)), processing_w_ - region_px0_);
  region_ph_ = std::min(static_cast<int>(std::rint(height * processing_scale_)), processing_h_ - region_py0_);
  if (region_pw_ <= 0 || region_ph_ <= 0) return false;
  region_screen_.left = client_rect.left + left;
  region_screen_.top = client_rect.top + top;
  region_screen_.right = region_screen_.left + width;
  region_screen_.bottom = region_screen_.top + height;
  region_active_ = true;
  grabber_client_ = client_rect;
  return true;
}

void ObserverApp::DroppedCaptureFrame() {
  ++capture_missing_frames_;
  if (capture_missing_frames_ <= 2) return;
  Report("status.doomsday.capture_failed");
  Stop();
}

void ObserverApp::RefreshTargetAlive(const imgproc::Mat8& frame) {
  for (auto& target : scene_.targets) {
    if (!target.alive) continue;
    double core = -1.0;
    const bool present = vision_.TargetPresent(frame, target, &core);
    // sliding-window verdict lives on the scene model
    auto& recent = alive_recent_[target.id];
    recent.push_back(present ? 0 : 1);
    if (recent.size() > static_cast<std::size_t>(kAliveMissWindow)) {
      recent.erase(recent.begin(), recent.begin() + (recent.size() - kAliveMissWindow));
    }
    int misses = 0;
    for (const int v : recent) misses += v;
    target.core_cyan = core;
    if (misses >= kAliveMissLimit) {
      target.alive = false;
    }
  }
}

void ObserverApp::ObserveFrame() {
  RECT client{};
  if (!GetGameClientRect(client)) {
    DroppedCaptureFrame();
    return;
  }
  double captureMs = 0, resizeMs = 0;
  std::vector<std::uint32_t> crop;
  int cropW = 0, cropH = 0;
  bool haveCrop = false;
  bool grabberAlive = false;
  std::string grabberError;
  {
    std::lock_guard<std::mutex> lock(grabber_mutex_);
    grabberAlive = grabber_thread_alive_;
    grabberError = grabber_error_;
    if (grabberAlive && grabberError.empty() && !grabber_frame_.empty() &&
        grabber_timestamp_ > taken_timestamp_) {
        crop = grabber_frame_;  // copy under lock (Python take() semantics)
      cropW = grabber_w_;
      cropH = grabber_h_;
      taken_timestamp_ = grabber_timestamp_;
      frame_taken_at_ = taken_timestamp_;
      captureMs = grabber_bitblt_ms_;
      haveCrop = true;
    }
  }
  if (!grabberAlive || !grabberError.empty()) {
    // the grabber quits on a moved/resized client or a failed BitBlt: fall
    // back to the serial path, which re-derives the region on the next frame
    StopGrabber();
    region_active_ = false;
    if (!haveCrop) return;
  } else if (!haveCrop) {
    return;  // no new pixels: no observation
  }
  bool fullFrameFallback = false;
  if (haveCrop) {
    // ingest the scaled region crop into the persistent processing frame
    if (region_pw_ == cropW && region_ph_ == cropH) {
      for (int y = 0; y < cropH; ++y) {
        const std::uint32_t* src = crop.data() + static_cast<std::size_t>(y) * cropW;
        std::uint8_t* dst = processing_frame_.Ptr(region_py0_ + y) + region_px0_ * 3;
        for (int x = 0; x < cropW; ++x) {
          dst[3 * x + 0] = static_cast<std::uint8_t>(src[x] & 0xFF);
          dst[3 * x + 1] = static_cast<std::uint8_t>((src[x] >> 8) & 0xFF);
          dst[3 * x + 2] = static_cast<std::uint8_t>((src[x] >> 16) & 0xFF);
        }
      }
      resizeMs = 0.2;
    } else {
      haveCrop = false;  // region mismatch: fall back
    }
  }
  if (!haveCrop) {
    if (!region_active_) {
      // re-derive region on the full path
      if (!ConfigureRegion(client)) {
        DroppedCaptureFrame();
        return;
      }
      (void)client;
    }
    GameFrame raw;
    const auto started = NowSeconds();
    const bool ok = CaptureGameFrameStatus(raw, nullptr) ==
                    gta5::capture::CaptureStatus::NewFrame;
    captureMs = (NowSeconds() - started) * 1000.0;
    if (!ok) {
      DroppedCaptureFrame();
      return;
    }
    if (raw.width != processing_w_ || raw.height != processing_h_) {
      processing_w_ = raw.width;
      processing_h_ = raw.height;
      processing_frame_ = imgproc::Mat8(processing_h_, processing_w_, 3);
    }
    for (int y = 0; y < processing_h_; ++y) {
      const std::uint32_t* src = raw.bgra.data() + static_cast<std::size_t>(y) * raw.width;
      std::uint8_t* dst = processing_frame_.Ptr(y);
      for (int x = 0; x < processing_w_; ++x) {
        dst[3 * x + 0] = static_cast<std::uint8_t>(src[x] & 0xFF);
        dst[3 * x + 1] = static_cast<std::uint8_t>((src[x] >> 8) & 0xFF);
        dst[3 * x + 2] = static_cast<std::uint8_t>((src[x] >> 16) & 0xFF);
      }
    }
    resizeMs = 0.0;
    frame_taken_at_ = NowSeconds();
    fullFrameFallback = true;
  }
  capture_missing_frames_ = 0;
  (void)fullFrameFallback;

  // exit detection: the sparse anchor gate is microseconds, so it runs every
  // frame; only after kIngameFastMissTolerance consecutive misses does the
  // expensive full gate confirm (transient artefacts never clear both LCDs).
  if (ingame_anchors_.valid) {
    if (vision_.IngameFast(processing_frame_, ingame_anchors_)) {
      ingame_missing_frames_ = 0;
    } else if (++ingame_missing_frames_ >= kIngameFastMissTolerance) {
      const vision::GateResult full =
          vision_.Ingame(processing_frame_, &ingame_anchors_);
      if (full.status != "YES") {
        Report("status.doomsday.exited");
        Stop();
        return;
      }
      ingame_missing_frames_ = 0;
    }
  } else if (frame_number_ % kIngameCheckPeriod == 0) {
    const vision::GateResult ingame =
        vision_.Ingame(processing_frame_, &ingame_anchors_);
    if (ingame.status != "YES") {
      if (++ingame_missing_frames_ >= 4) {
        Report("status.doomsday.exited");
        Stop();
      }
      return;
    }
    ingame_missing_frames_ = 0;
  }

  const double nowPc = NowSeconds();
  static double lastFramePc = nowPc;
  const double dtMs = (nowPc - lastFramePc) * 1000.0;
  lastFramePc = nowPc;
  if (dtMs > 1.0) {
    const double instant = 1000.0 / dtMs;
    fps_ema_ = fps_ema_ == 0 ? instant : fps_ema_ * 0.9 + instant * 0.1;
  }

  const tracking::TrackingFrame tracked = tracker_->Update(processing_frame_);
  last_tracked_ = std::make_unique<tracking::TrackingFrame>(tracked);
  // evidence dump: when a mirror goes unreadable mid-run, save the whole
  // processing frame once so MirrorLongEdgePair can be replayed offline
  for (const auto& observed : tracked.mirrors) {
    if (observed.misses == kLostFrameDumpAt &&
        lost_dumped_.insert(observed.mirror_id).second) {
    }
  }
  for (auto& mirror : scene_.mirrors) {
    for (const auto& observed : tracked.mirrors) {
      if (observed.mirror_id == mirror.id && observed.misses == 0 &&
          observed.has_angle) {
        mirror.observed_angle_deg = observed.angle_deg;
      }
    }
  }
  // ghost dump (user rule 2026-09-26): ANY mirror whose angle jumps more
  // than kGhostDumpJumpDeg between two of its measurements is not physical —
  // save the whole processing frame immediately so the misread can be
  // replayed offline. No selected-only filter: idle remeasures count too.
  for (const auto& observed : tracked.mirrors) {
    if (!observed.has_angle || observed.misses != 0) continue;
    const auto it = last_sel_angle_.find(observed.mirror_id);
    if (it != last_sel_angle_.end() && ghost_dumps_ < kGhostDumpCap) {
      const double jump =
          std::fmod(observed.angle_deg - it->second + 270.0, 180.0) - 90.0;
      if (std::fabs(jump) > kGhostDumpJumpDeg) {
        ++ghost_dumps_;
      }
    }
    last_sel_angle_[observed.mirror_id] = observed.angle_deg;
  }
  RefreshTargetAlive(processing_frame_);

  // F6 one-key: fire the auto attack once tracking has produced a frame
  if (auto_attack_pending_) {
    auto_attack_pending_ = false;
    if (attack_state_ == "IDLE") {
      StartAttack();
      if (attack_state_ == "IDLE") {
        Stop();  // failed (match/preset): report and stand by
        return;
      }
    }
  }

  // preset attack: the motion layer runs on THIS frame loop (the only
  // thread allowed to touch input), the Lua runner pumps from its own thread.
  if (attack_state_ == "PRESET_RUN") {
    const double frameAge =
        std::max(0.0, NowSeconds() - frame_taken_at_);
    preset_motion_->Tick(last_tracked_.get(), frameAge);
    WritePresetFrame(tracked);
    if (preset_runner_ != nullptr && preset_runner_->JoinIfFinished()) {
      const std::string error = preset_runner_->error();
      preset_runner_.reset();
      if (!error.empty()) FailAttack("PRESET_" + error);
      else PresetFinished();
      if (!active_) return;  // one-key flow wound the observer down
    }
  }

  if (overlay_hwnd_ && overlay_enabled_ && overlay_enabled_()) {
    // re-asserting HWND_TOPMOST every frame churns DWM and blocks on the UI
    // thread; only reposition when the client rect actually moved
    if (!overlay_positioned_ || !EqualRect(&client, &overlay_client_)) {
      SetWindowPos(overlay_hwnd_, HWND_TOPMOST, client.left, client.top,
                   client.right - client.left, client.bottom - client.top,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
      overlay_client_ = client;
      overlay_positioned_ = true;
    }
    const double toClient = 1.0 / processing_scale_;
    struct Entity { double x, y, r; };
    std::vector<Entity> entities;
    for (const auto& m : scene_.mirrors) {
      entities.push_back({m.cx * toClient, m.cy * toClient, m.radius * toClient});
    }
    for (const auto& a : scene_.auto_mirrors) {
      entities.push_back({a.cx * toClient, a.cy * toClient, a.radius * toClient});
    }
    for (const auto& t : scene_.targets) {
      entities.push_back({t.cx * toClient, t.cy * toClient, t.radius * toClient});
    }
    const int mirrorCount = static_cast<int>(scene_.mirrors.size());
    const int autoCount = static_cast<int>(scene_.auto_mirrors.size());
    std::map<std::string, int> mirrorEntity;
    for (int i = 0; i < mirrorCount; ++i) {
      mirrorEntity[scene_.mirrors[i].id] = i;
    }

    std::vector<overlay::Label> labels;
    std::vector<int> labelEntity;
    for (const auto& mirror : tracked.mirrors) {
      overlay::Label label;
      std::string text = mirror.mirror_id + " --";
      if (mirror.has_angle && mirror.misses == 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%6.2f", mirror.angle_deg);
        text = mirror.mirror_id + " " + buf;
      } else if (mirror.misses) {
        text = mirror.mirror_id + " LOST:" + std::to_string(mirror.misses);
      }
      if (mirror.selected) text = "> " + text + " <";
      label.text = text;
      label.x = mirror.cx * toClient;
      label.y = mirror.cy * toClient;
      label.radius = mirror.radius * toClient;
      label.angle_deg = mirror.misses == 0 ? mirror.angle_deg : -1;
      label.color = RGB(128, 255, 128);
      const auto it = mirrorEntity.find(mirror.mirror_id);
      labelEntity.push_back(it != mirrorEntity.end() ? it->second : -1);
      labels.push_back(label);
    }
    for (int i = 0; i < autoCount; ++i) {
      const auto& autoMirror = scene_.auto_mirrors[i];
      overlay::Label label;
      label.text = autoMirror.id + " oct";
      label.x = autoMirror.cx * toClient;
      label.y = autoMirror.cy * toClient;
      label.radius = autoMirror.radius * toClient;
      label.color = RGB(255, 210, 0);
      labelEntity.push_back(mirrorCount + i);
      labels.push_back(label);
    }
    for (int i = 0; i < static_cast<int>(scene_.targets.size()); ++i) {
      const auto& target = scene_.targets[i];
      overlay::Label label;
      label.text = target.id + (target.alive ? "" : " X");
      label.x = target.cx * toClient;
      label.y = target.cy * toClient;
      label.radius = target.radius * toClient;
      label.color = target.alive ? RGB(255, 195, 79) : RGB(90, 90, 255);
      labelEntity.push_back(mirrorCount + autoCount + i);
      labels.push_back(label);
    }

    struct TextRect { double x0, y0, x1, y1; };
    const double fontScale = (client.bottom - client.top) / geom::kRefHeight;
    const double kCharW = 16.0 * fontScale;
    const double kTextH = 30.0 * fontScale;
    const double kGap = 6.0 * fontScale;
    auto coversDisk = [](const TextRect& t, const Entity& e) {
      const double px = std::max(t.x0, std::min(e.x, t.x1));
      const double py = std::max(t.y0, std::min(e.y, t.y1));
      const double dx = e.x - px, dy = e.y - py;
      return dx * dx + dy * dy < e.r * e.r;
    };
    std::vector<int> placed;
    for (std::size_t i = 0; i < labels.size(); ++i) {
      overlay::Label& label = labels[i];
      const double w = static_cast<double>(label.text.size()) * kCharW;
      const double cx = label.x, cy = label.y, r = label.radius;
      const TextRect candidates[] = {
          {cx - w / 2, cy - r - kGap - kTextH, cx + w / 2, cy - r - kGap},
          {cx - r - kGap - w, cy - kTextH / 2, cx - r - kGap, cy + kTextH / 2},
          {cx + r + kGap, cy - kTextH / 2, cx + r + kGap + w, cy + kTextH / 2},
          {cx - w / 2, cy + r + kGap, cx + w / 2, cy + r + kGap + kTextH},
      };
      TextRect chosen = candidates[0];
      for (const TextRect& cand : candidates) {
        bool blocked = false;
        for (int e = 0; e < static_cast<int>(entities.size()); ++e) {
          if (e == labelEntity[i]) continue;
          if (coversDisk(cand, entities[e])) { blocked = true; break; }
        }
        if (!blocked) {
          for (int j : placed) {
            const Entity& other = entities[j];
            const Entity excl{other.x, other.y, other.r * 1.5};
            if (coversDisk(cand, excl)) { blocked = true; break; }
          }
        }
        if (!blocked) { chosen = cand; break; }
      }
      label.text_at_pos = true;
      label.text_x = (chosen.x0 + chosen.x1) / 2.0;
      label.text_y = chosen.y0;
      if (labelEntity[i] >= 0) placed.push_back(labelEntity[i]);
    }

    char hud[32];
    std::snprintf(hud, sizeof(hud), "%.0fhz", fps_ema_);
    {
      std::lock_guard<std::mutex> lock(overlay_mutex_);
      overlay_labels_ = std::move(labels);
      overlay_hud_ = hud;
    }
    overlay_cleared_ = false;
    InvalidateRect(overlay_hwnd_, nullptr, TRUE);
  } else {
    ClearOverlay();
  }

  ++frame_number_;
}

void ObserverApp::ApplyBinding(const matcher::MatchResult& match) {
  // relabel scene detections with the authored level's M/T ids (Python
  // _apply_authored_ids); backfill missing targets from the template
  for (const auto& [index_text, authored] : match.mirrors) {
    const int index = index_text;
    if (index >= 0 && index < static_cast<int>(scene_.mirrors.size())) {
      scene_.mirrors[index].id = authored;
    }
  }
  for (const auto& [index_text, authored] : match.targets) {
    const int index = index_text;
    if (index >= 0 && index < static_cast<int>(scene_.targets.size())) {
      scene_.targets[index].id = authored;
    }
  }
  if (!scene_.targets.empty() && !match.missing_targets.empty()) {
    double radiusSum = 0;
    for (const auto& t : scene_.targets) radiusSum += t.radius;
    const double meanRadius = radiusSum / scene_.targets.size();
    for (const auto& missing : match.missing_targets) {
      bool known = false;
      for (const auto& t : scene_.targets) {
        if (t.id == missing.id) { known = true; break; }
      }
      if (known) continue;
      vision::Target t;
      t.id = missing.id;
      t.cx = missing.center_x;
      t.cy = missing.center_y;
      t.radius = meanRadius;
      t.rect[0] = std::round(missing.center_x - meanRadius);
      t.rect[1] = std::round(missing.center_y - meanRadius);
      t.rect[2] = std::round(meanRadius * 2);
      t.rect[3] = std::round(meanRadius * 2);
      t.alive = true;
      scene_.targets.push_back(t);
    }
  }
  level_binding_ = match;
  has_binding_ = true;
}

void ObserverApp::StartAttack() {
  if (!active_ || tracker_ == nullptr) return;
  game_input_->ReleaseAll();
  preset_motion_->RearmSelectionObservation();

  std::vector<std::pair<double, double>> mirrors, targets;
  for (const auto& m : scene_.mirrors) mirrors.push_back({m.cx, m.cy});
  for (const auto& t : scene_.targets) targets.push_back({t.cx, t.cy});
  matcher::MatchResult match;
  try {
    match = matcher::MatchLevel(mirrors, targets, scene_.playfield);
  } catch (const std::exception& exc) {
    (void)exc;
    FailAttack("level match failed");
    return;
  }
  ApplyBinding(match);

  RECT client{};
  if (!GetGameClientRect(client)) {
    FailAttack("client rect lost");
    return;
  }
  preset_motion_->Bind(static_cast<double>(client.left),
                       static_cast<double>(client.top),
                       static_cast<double>(scene_.playfield[0]),
                       1.0 / processing_scale_);
  const int level = match.level;
  std::string presetCode;
  const std::string presetName = level < 10 ? "presets/level0" + std::to_string(level) + ".lua"
                                           : "presets/level" + std::to_string(level) + ".lua";
  const auto embeddedPreset = doomsday_embedded::Find(presetName);
  if (!embeddedPreset.empty()) presetCode.assign(embeddedPreset.data(), embeddedPreset.size());
  if (presetCode.empty()) {
    FailAttack("preset unavailable");
    return;
  }

  auto runner = std::make_unique<preset::PresetRunner>(
      this, preset_motion_.get(), match,
      preset::PresetRunner::ScanWaitLookaheads(presetCode),
      std::string());
  std::string error;
  if (!runner->Start(presetCode, &error)) {
    (void)error;
    FailAttack("preset start failed");
    return;
  }
  preset_runner_ = std::move(runner);
  attack_state_ = "PRESET_RUN";
}

void ObserverApp::CancelAttack(const std::string& reason, bool report) {
  (void)reason;
  const bool wasRunning = attack_state_ != "IDLE";
  if (preset_runner_ != nullptr) {
    preset_runner_->Stop();
    preset_runner_.reset();
  }
  preset_motion_->Cancel();
  game_input_->ReleaseAll();
  attack_state_ = "IDLE";
  if (report && wasRunning) {
    Report("status.doomsday.stopped");
  }
}

void ObserverApp::FailAttack(const std::string& reason) {
  (void)reason;
  if (preset_runner_ != nullptr) {
    preset_runner_->Stop();
    preset_runner_.reset();
  }
  preset_motion_->Cancel();
  game_input_->ReleaseAll();
  attack_state_ = "IDLE";
  Report("status.doomsday.failed");
  Stop();  // one-key flow: terminal state returns to standby
}

void ObserverApp::PresetFinished() {
  if (attack_state_ != "PRESET_RUN") return;
  preset_runner_.reset();
  preset_motion_->Cancel();
  game_input_->ReleaseAll();
  attack_state_ = "IDLE";
  completed_ = true;
  Report("status.doomsday.completed");
  Stop();  // one-key flow: clear overlay, stand by
}

void ObserverApp::WritePresetFrame(const tracking::TrackingFrame& tracked) {
  if (preset_runner_ == nullptr) return;
  Json record = Json::Object();
  record.Set("n", Json::Number(static_cast<double>(frame_number_)));
  Json mirrorsJson = Json::Object();
  for (const auto& m : tracked.mirrors) {
    Json entry = Json::Object();
    entry.Set("angle", Json::Number(m.has_angle ? m.angle_deg : 0.0));
    entry.Set("snapped", Json::Number(m.has_angle ? m.snapped_angle_deg : 0.0));
    entry.Set("misses", Json::Number(m.misses));
    entry.Set("selected", Json::Bool(m.selected));
    mirrorsJson.Set(m.mirror_id, entry);
  }
  record.Set("mirrors", mirrorsJson);
  Json targetsJson = Json::Object();
  for (const auto& t : scene_.targets) {
    Json entry = Json::Object();
    entry.Set("alive", Json::Bool(t.alive));
    entry.Set("core", Json::Number(t.core_cyan));
    targetsJson.Set(t.id, entry);
  }
  record.Set("targets", targetsJson);
  record.Set("motion_status", Json::String(preset_motion_->status_text()));
  record.Set("op", Json::String(preset_runner_->current_op_name()));
  preset_runner_->log()->Frame(record);
}

void ObserverApp::Stop() {
  const bool wasActive = active_.exchange(false);
  auto_attack_pending_ = false;
  CancelAttack("observation stopped", false);
  ClearOverlay();
  StopGrabber();
  tracker_storage_.reset();
  tracker_ = nullptr;
  scene_ = vision::SceneModel{};
  alive_recent_.clear();
  lost_dumped_.clear();
  capture_missing_frames_ = 0;
  ingame_missing_frames_ = 0;
  if (wasActive) Report("status.doomsday.stopped");
}

bool ObserverApp::RunSession(const std::function<bool()>& stopRequested,
                             const std::function<bool()>& overlayEnabled,
                             const std::function<void(const std::wstring&)>& status) {
  stop_requested_ = stopRequested;
  overlay_enabled_ = overlayEnabled;
  status_ = status;
  completed_ = false;
  game_input_ = std::make_unique<input::HeldKeyInput>();
  preset_motion_ = std::make_unique<motion::PresetMotion>(game_input_.get());
  if (!Start()) {
    Stop();
    return false;
  }
  while (active_ && !(stop_requested_ && stop_requested_())) {
    if (active_ && scheduler_.Due()) {
      const bool hasNew = [this] {
        std::lock_guard<std::mutex> lock(grabber_mutex_);
        return !grabber_frame_.empty() && grabber_timestamp_ > taken_timestamp_;
      }();
      if (hasNew) ObserveFrame();
    }
    Sleep(active_ ? 1 : 30);
  }
  if (stop_requested_ && stop_requested_()) Stop();
  const bool completed = completed_;
  Stop();
  return completed;
}

bool ObserverApp::DetectInGame(const gta5::capture::GameFrame& frame) {
  if (frame.bgra.empty()) {
    ResetInGameCache();
    return false;
  }
  if (have_pending_ingame_ &&
      cache_window_generation_ == frame.windowGeneration &&
      cache_width_ == frame.width && cache_height_ == frame.height) {
    return true;
  }
  // cheap path: cached anchors still read as the minigame on this frame
  if (ingame_anchors_.valid &&
      cache_window_generation_ == frame.windowGeneration &&
      cache_width_ == frame.width && cache_height_ == frame.height &&
      vision_.IngameFastBgra(frame.bgra.data(), frame.width, frame.height,
                             ingame_anchors_)) {
    pending_frame_ = frame;
    have_pending_ingame_ = true;
    return true;
  }
  imgproc::Mat8 bgr = FrameToBgr(frame);
  const vision::GateResult ingame = vision_.Ingame(bgr, &ingame_anchors_);
  if (ingame.status != "YES") {
    ResetInGameCache();
    return false;
  }
  pending_frame_ = frame;
  have_pending_ingame_ = true;
  cache_window_generation_ = frame.windowGeneration;
  cache_width_ = frame.width;
  cache_height_ = frame.height;
  for (int i = 0; i < 4; ++i) pending_ingame_rect_[i] = ingame.rect[i];
  return true;
}

void ObserverApp::ResetInGameCache() {
  have_pending_ingame_ = false;
  pending_frame_ = gta5::capture::GameFrame{};
  cache_window_generation_ = 0;
  cache_width_ = 0;
  cache_height_ = 0;
}

bool DetectInGame(const gta5::capture::GameFrame& frame) {
  return ObserverApp::Instance().DetectInGame(frame);
}

void ResetInGameCache() {
  ObserverApp::Instance().ResetInGameCache();
}

bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<bool()>& overlayEnabled,
                const std::function<void(const std::wstring&)>& status) {
  return ObserverApp::Instance().RunSession(stopRequested, overlayEnabled, status);
}

void SetOverlayWindow(HWND hwnd) {
  ObserverApp::Instance().SetOverlayWindow(hwnd);
}

void ClearOverlay() {
  ObserverApp::Instance().ClearOverlay();
}

LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  return ObserverApp::Instance().OverlayWindowProc(hwnd, msg, wp, lp);
}

}  // namespace gta5::games::doomsday_unlock
