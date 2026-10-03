#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <propidl.h>

#include <atomic>
#include <chrono>
#include <cwchar>
#include <memory>
#include <string>
#include <thread>

#include "games/games.h"
#include "app/app_ui.h"
#include "app/app_runtime.h"
#include "app/localization.h"
#include "capture/game_window.h"
#include "input/key_input.h"
#include "resources/resource.h"

namespace {

std::wstring T(const char* key) { return gta5::app::l10n::Text(key); }

constexpr UINT kMsgLog = WM_APP + 1;
constexpr UINT kMsgStatus = WM_APP + 2;
constexpr UINT kMsgWorkerDone = WM_APP + 3;

DWORD ElevatedRestartParentId(const wchar_t* commandLine) {
  constexpr wchar_t prefix[] = L"--elevated-restart=";
  if (!commandLine) return 0;
  while (*commandLine == L' ' || *commandLine == L'\t') ++commandLine;
  if (wcsncmp(commandLine, prefix, std::size(prefix) - 1) != 0) return 0;

  wchar_t* end = nullptr;
  const unsigned long processId = wcstoul(commandLine + std::size(prefix) - 1, &end, 10);
  return processId && end && *end == L'\0' ? static_cast<DWORD>(processId) : 0;
}

bool HasPluginArgument(const wchar_t* commandLine) {
  if (!commandLine) return false;
  const wchar_t* cursor = commandLine;
  while (*cursor) {
    while (*cursor == L' ' || *cursor == L'\t') ++cursor;
    if (!*cursor) break;
    const wchar_t* begin = cursor;
    while (*cursor && *cursor != L' ' && *cursor != L'\t') ++cursor;
    if (static_cast<std::size_t>(cursor - begin) == 7 &&
        _wcsnicmp(begin, L"-plugin", 7) == 0) {
      return true;
    }
  }
  return false;
}

HWND g_host = nullptr;
HICON g_appIcon = nullptr;
HANDLE g_singleInstanceMutex = nullptr;
HWND g_cursorOverlay = nullptr;
HWND g_marksOverlay = nullptr;
HWND g_flashingOverlay = nullptr;
HWND g_chooseFingerprintOverlay = nullptr;
HWND g_sortFingerprintOverlay = nullptr;
HWND g_matchOverlay = nullptr;
HWND g_doomsdayUnlockOverlay = nullptr;
bool g_applyModeWhenWorkerStops = false;
bool g_pluginMode = false;
std::atomic<bool> g_pluginSessionStarted{false};
std::atomic<bool> g_pluginDetectionTimedOut{false};

enum class GameKind {
  None,
  Slider,
  Flashing,
  ChooseFingerprint,
  SortFingerprint,
  Fleeca,
  FindNumber,
  Match,
  DoomsdayUnlock,
};

std::wstring GameName(GameKind game) {
  switch (game) {
    case GameKind::Slider:
      return T("game.slider");
    case GameKind::Flashing:
      return T("game.flashing");
    case GameKind::ChooseFingerprint:
      return T("game.choose_fingerprint");
    case GameKind::SortFingerprint:
      return T("game.sort_fingerprint");
    case GameKind::Fleeca:
      return T("game.fleeca");
    case GameKind::FindNumber:
      return T("game.find_number");
    case GameKind::Match:
      return T("game.match");
    case GameKind::DoomsdayUnlock:
      return T("game.doomsday_unlock");
    default:
      return T("game.none");
  }
}

void HideAllGameOverlays() {
  gta5::games::slider::HideTransientOverlays();
  gta5::games::flashing::HideOverlay();
  gta5::games::choose_fingerprint::ClearOverlay();
  gta5::games::sort_fingerprint::ClearOverlay();
  gta5::games::match::ClearOverlay();
  gta5::games::doomsday_unlock::ClearOverlay();
}

void ResetAllInGameCaches() {
  gta5::games::slider::ResetInGameCache();
  gta5::games::flashing::ResetInGameCache();
  gta5::games::choose_fingerprint::ResetInGameCache();
  gta5::games::sort_fingerprint::ResetInGameCache();
  gta5::games::fleeca::ResetInGameCache();
  gta5::games::find_number::ResetInGameCache();
  gta5::games::match::ResetInGameCache();
  gta5::games::doomsday_unlock::ResetInGameCache();
}

GameKind DetectGame(const gta5::capture::GameFrame& frame) {
  if (gta5::games::slider::DetectInGame(frame)) return GameKind::Slider;
  if (gta5::games::flashing::DetectInGame(frame)) return GameKind::Flashing;
  if (gta5::games::choose_fingerprint::DetectInGame(frame)) return GameKind::ChooseFingerprint;
  if (gta5::games::sort_fingerprint::DetectInGame(frame)) return GameKind::SortFingerprint;
  if (gta5::games::fleeca::DetectInGame(frame)) return GameKind::Fleeca;
  // Its blue-bar signature is intentionally last so it cannot shadow older games.
  if (gta5::games::find_number::DetectInGame(frame)) return GameKind::FindNumber;
  if (gta5::games::match::DetectInGame(frame)) return GameKind::Match;
  if (gta5::games::doomsday_unlock::DetectInGame(frame)) return GameKind::DoomsdayUnlock;
  return GameKind::None;
}

void PostStatus(const std::wstring& text) {
  auto* payload = new std::wstring(text);
  if (g_host) PostMessageW(g_host, kMsgStatus, 0, reinterpret_cast<LPARAM>(payload));
  else delete payload;
}

void PostLog(const std::wstring& text) {
  auto* payload = new std::wstring(text);
  if (g_host) PostMessageW(g_host, kMsgLog, 0, reinterpret_cast<LPARAM>(payload));
  else delete payload;
}

std::wstring LocalizeRuntimeStatus(const std::wstring& text) {
  struct StatusTranslation {
    const wchar_t* source;
    const char* key;
  };
  const StatusTranslation translations[] = {
      {L"analysis latency too high; stopped", "status.slider_latency"},
      {L"capture failed", "status.capture_failed"},
      {L"active bar timeout; stopped", "status.slider_timeout"},
      {L"completed; stopped", "status.completed"},
      {L"waiting yellow outline", "status.waiting_yellow"},
      {L"in minigame", "status.in_minigame"},
      {L"stopped", "status.stopped"},
      {L"searching minigame", "status.search_minigame"},
      {L"fingerprint: locating", "status.fingerprint_locating"},
      {L"fingerprint: exited", "status.fingerprint_exited"},
      {L"fingerprint: confirming exit", "status.fingerprint_confirm_exit"},
      {L"fingerprint: level complete", "status.fingerprint_complete"},
      {L"fingerprint: auto input", "status.fingerprint_input"},
      {L"fingerprint: waiting next level", "status.fingerprint_next"},
      {L"sort_fingerprint: locating", "status.sort_locating"},
      {L"sort_fingerprint: analyzing", "status.sort_analyzing"},
      {L"sort_fingerprint: waiting next round", "status.sort_next"},
      {L"sort_fingerprint: analyzing next round", "status.sort_analyzing_next"},
      {L"sort_fingerprint: round complete", "status.sort_complete"},
      {L"sort_fingerprint: verifying input", "status.sort_verifying"},
      {L"flashing: locating", "status.flashing_locating"},
      {L"flashing: next level", "status.flashing_next_level"},
      {L"flashing: exited", "status.flashing_exited"},
      {L"flashing: confirming exit", "status.flashing_confirm_exit"},
      {L"flashing: waiting next level", "status.flashing_wait_next"},
      {L"flashing: verifying column", "status.flashing_verify"},
      {L"flashing: reading pattern", "status.flashing_read"},
      {L"flashing: auto input", "status.flashing_input"},
      {L"fleeca: planning route", "status.fleeca_planning"},
      {L"fleeca: starting signal", "status.fleeca_starting"},
      {L"fleeca: navigating", "status.fleeca_navigating"},
      {L"fleeca: waiting for result", "status.fleeca_wait_result"},
      {L"fleeca: detecting result", "status.fleeca_detect_result"},
      {L"fleeca: result detected; waiting for game", "status.fleeca_result_delay"},
      {L"fleeca: advancing", "status.fleeca_advancing"},
      {L"fleeca: waiting for next round", "status.fleeca_wait_next"},
      {L"fleeca: next round", "status.fleeca_next"},
      {L"fleeca: completed", "status.fleeca_completed"},
      {L"fleeca: capture failed", "status.capture_failed"},
      {L"fleeca: route unavailable", "status.fleeca_route_failed"},
      {L"fleeca: signal not detected", "status.fleeca_signal_failed"},
      {L"fleeca: direction not confirmed", "status.fleeca_direction_failed"},
      {L"fleeca: result not detected", "status.fleeca_result_failed"},
      {L"find_number: locating", "status.find_number_locating"},
      {L"find_number: analyzing", "status.find_number_analyzing"},
      {L"find_number: moving", "status.find_number_moving"},
      {L"find_number: verifying position", "status.find_number_verifying"},
      {L"find_number: submitting", "status.find_number_submitting"},
      {L"find_number: completed", "status.find_number_complete"},
      {L"find_number: minigame exited", "status.find_number_exited"},
      {L"match: locating", "status.match_locating"},
      {L"match: analyzing", "status.match_analyzing"},
      {L"match: reading puzzle", "status.match_reading"},
      {L"match: connecting", "status.match_connecting"},
      {L"match: verifying connection", "status.match_verifying"},
      {L"match: completed", "status.match_complete"},
      {L"match: minigame exited", "status.match_exited"},
      {L"match: no solution", "status.match_no_solution"},
  };
  for (const StatusTranslation& translation : translations) {
    if (text == translation.source) return T(translation.key);
  }
  return text;
}

HICON LoadAppIcon(HINSTANCE inst) {
  return LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP_ICON));
}

void ApplyWindowIcon(HWND hwnd) {
  if (!hwnd || !g_appIcon) return;
  SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon));
  SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIcon));
}

void WorkerMain() {
  using Clock = std::chrono::steady_clock;
  constexpr DWORD kDetectionPollIntervalMs = 30;
  gta5::app::runtime::ConfigureLatencySensitiveThread();
  PostLog(T("log.start"));
  PostStatus(T("status.search_game"));
  HideAllGameOverlays();

  const auto deadline = Clock::now() + std::chrono::seconds(20);
  bool completed = false;
  bool captureFailed = false;
  bool gameWindowFound = false;
  while (!gta5::app::runtime::StopRequested() && Clock::now() < deadline) {
    if (!gameWindowFound && !gta5::capture::FindGameWindow()) {
      PostStatus(T("status.search_game"));
      Sleep(100);
      continue;
    }
    if (!gameWindowFound) {
      PostLog(T("log.found_game"));
      PostStatus(T("status.search_minigame"));
      gameWindowFound = true;
    }
    gta5::capture::GameFrame detectionFrame;
    const gta5::capture::CaptureStatus captureStatus =
        gta5::capture::CaptureGameFrameStatus(detectionFrame);
    if (captureStatus == gta5::capture::CaptureStatus::NoNewFrame) {
      Sleep(kDetectionPollIntervalMs);
      continue;
    }
    if (captureStatus == gta5::capture::CaptureStatus::Error) {
      captureFailed = true;
      PostLog(gta5::capture::CurrentBackend() == gta5::capture::Backend::DxgiExperimental
                  ? L"DXGI capture failed; no fallback was used"
                  : L"GDI capture failed");
      PostStatus(T("status.capture_failed"));
      break;
    }
    if (captureStatus != gta5::capture::CaptureStatus::NewFrame) {
      continue;
    }
    GameKind game = DetectGame(detectionFrame);
    if (game == GameKind::None) {
      Sleep(kDetectionPollIntervalMs);
      continue;
    }

    if (g_pluginMode) g_pluginSessionStarted.store(true, std::memory_order_relaxed);
    PostLog(T("status.running") + L" " + GameName(game));
    PostStatus(T("status.running") + L" " + GameName(game));
    switch (game) {
      case GameKind::Slider:
        gta5::games::slider::RunSession(
            [] { return gta5::app::runtime::StopRequested(); });
        completed = true;
        break;
      case GameKind::Flashing:
        completed = gta5::games::flashing::RunSession(
            [] { return gta5::app::runtime::StopRequested(); },
            [] { return gta5::app::ui::OverlayEnabled(); },
            [](const std::wstring& text) { PostStatus(text); });
        break;
      case GameKind::ChooseFingerprint:
        completed = gta5::games::choose_fingerprint::RunSession(
            [] { return gta5::app::runtime::StopRequested(); },
            [] { return gta5::app::ui::OverlayEnabled(); },
            [](const std::wstring& text) { PostStatus(text); });
        break;
      case GameKind::SortFingerprint:
        completed = gta5::games::sort_fingerprint::RunSession(
            [] { return gta5::app::runtime::StopRequested(); },
            [] { return gta5::app::ui::OverlayEnabled(); },
            [](const std::wstring& text) { PostStatus(text); },
            [](const std::wstring& text) { PostLog(text); });
        break;
      case GameKind::Fleeca:
        completed = gta5::games::fleeca::RunSession(
            [] { return gta5::app::runtime::StopRequested(); },
            [](const std::wstring& text) { PostStatus(text); });
        break;
      case GameKind::FindNumber:
        completed = gta5::games::find_number::RunSession(
            [] { return gta5::app::runtime::StopRequested(); },
            [](const std::wstring& text) { PostStatus(text); });
        break;
      case GameKind::Match:
        completed = gta5::games::match::RunSession(
            [] { return gta5::app::runtime::StopRequested(); },
            [] { return gta5::app::ui::OverlayEnabled(); },
            [](const std::wstring& text) { PostStatus(text); });
        break;
      case GameKind::DoomsdayUnlock:
        completed = gta5::games::doomsday_unlock::RunSession(
            [] { return gta5::app::runtime::StopRequested(); },
            [] { return gta5::app::ui::OverlayEnabled(); },
            [](const std::wstring& text) { PostStatus(text); });
        break;
      default:
        break;
    }
    break;
  }

  gta5::input::CancelAll();

  if (!completed && !gta5::app::runtime::StopRequested() && Clock::now() >= deadline) {
    if (g_pluginMode) {
      g_pluginDetectionTimedOut.store(true, std::memory_order_relaxed);
    }
    PostLog(gameWindowFound ? L"timeout: no supported minigame detected in 20s"
                            : L"timeout: GTA5 window not found in 20s");
    PostStatus(gameWindowFound ? T("status.detect_timeout") : T("status.game_timeout"));
  } else if (!captureFailed) {
    PostStatus(completed ? T("status.completed") : T("status.stopped"));
  }

  HideAllGameOverlays();
  ResetAllInGameCaches();
  gta5::capture::ClearGameWindow();
  gta5::app::runtime::SetRunning(false);
  gta5::app::ui::SetRunning(false);
  PostMessageW(g_host, kMsgWorkerDone, 0, 0);
}

void StartWorker() {
  if (gta5::app::runtime::Running()) return;
  g_pluginSessionStarted.store(false, std::memory_order_relaxed);
  g_pluginDetectionTimedOut.store(false, std::memory_order_relaxed);
  gta5::input::CancelAll();
  ResetAllInGameCaches();
  gta5::input::ConfigureSequenceTiming(
      std::chrono::milliseconds(gta5::app::ui::TapHoldMs()),
      std::chrono::milliseconds(gta5::app::ui::TapGapMs()));
  gta5::app::runtime::ResetStopRequest();
  gta5::games::slider::HideTransientOverlays();
  gta5::app::runtime::SetRunning(true);
  gta5::app::ui::SetRunning(true);
  gta5::app::ui::SetStatusText(T("status.starting"));
  PostStatus(T("status.starting"));
  gta5::app::ui::Repaint();
  gta5::app::runtime::WorkerThread() = std::thread(WorkerMain);
  gta5::app::ui::Repaint();
}

void StopWorker() {
  if (!gta5::app::runtime::Running()) return;
  gta5::app::ui::SetStatusText(T("status.stopping"));
  PostStatus(T("status.stopping"));
  gta5::app::ui::Repaint();
  gta5::app::runtime::RequestStop();
  gta5::input::CancelAll();
  HideAllGameOverlays();
}

void DestroyGameOverlayWindows();
void ApplyWindowMode(HINSTANCE inst);

void ToggleAutomation() {
  if (gta5::app::runtime::Running()) StopWorker();
  else {
    gta5::app::ui::CollapseHud();
    StartWorker();
  }
  gta5::app::ui::ShowToggleNotification(gta5::app::runtime::Running());
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == gta5::app::ui::ModeChangedMessage()) {
    g_applyModeWhenWorkerStops = gta5::app::runtime::Running();
    StopWorker();
    if (!g_applyModeWhenWorkerStops) PostMessageW(hwnd, WM_APP + 21, 0, 0);
    return 0;
  }
  if (msg == WM_APP + 21) {
    ApplyWindowMode(GetModuleHandleW(nullptr));
    return 0;
  }
  switch (msg) {
    case WM_CREATE:
      gta5::app::ui::RegisterRawKeyboardInput(hwnd);
      return 0;
    case WM_INPUT: {
      if (g_pluginMode) return 0;
      UINT size = 0;
      if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, nullptr, &size,
                          sizeof(RAWINPUTHEADER)) != 0 || size == 0) {
        return 0;
      }
      std::unique_ptr<BYTE[]> input(new BYTE[size]);
      if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, input.get(), &size,
                          sizeof(RAWINPUTHEADER)) != size) {
        return 0;
      }
      const RAWINPUT& raw = *reinterpret_cast<const RAWINPUT*>(input.get());
      if (raw.header.dwType != RIM_TYPEKEYBOARD || raw.data.keyboard.VKey == 255) return 0;

      static bool hotkeyDown = false;
      const int vk = static_cast<int>(raw.data.keyboard.VKey);
      const bool keyUp = (raw.data.keyboard.Flags & RI_KEY_BREAK) != 0;
      if (gta5::app::ui::IsListeningHotkey()) {
        if (!keyUp) gta5::app::ui::CaptureHotkeyVk(vk);
        return 0;
      }
      if (vk != gta5::app::ui::HotkeyVk()) return 0;
      if (keyUp) {
        hotkeyDown = false;
      } else if (!hotkeyDown) {
        hotkeyDown = true;
        ToggleAutomation();
      }
      return 0;
    }
    case kMsgLog: {
      std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
      OutputDebugStringW(text->c_str());
      OutputDebugStringW(L"\n");
      gta5::app::ui::SetLogText(*text);
      gta5::app::ui::Repaint();
      return 0;
    }
    case kMsgStatus: {
      std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
      gta5::app::ui::SetStatusText(LocalizeRuntimeStatus(*text));
      gta5::app::ui::Repaint();
      return 0;
    }
    case kMsgWorkerDone: {
      gta5::app::ui::SetRunning(false);
      auto& worker = gta5::app::runtime::WorkerThread();
      if (worker.joinable()) worker.detach();
      if (g_applyModeWhenWorkerStops) {
        g_applyModeWhenWorkerStops = false;
        PostMessageW(hwnd, WM_APP + 21, 0, 0);
      }
      if (g_pluginMode &&
          (g_pluginSessionStarted.load(std::memory_order_relaxed) ||
           g_pluginDetectionTimedOut.load(std::memory_order_relaxed))) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
      }
      gta5::app::ui::Repaint();
      return 0;
    }
    case WM_CLOSE:
      StopWorker();
      if (gta5::app::runtime::WorkerThread().joinable()) {
        gta5::app::runtime::WorkerThread().detach();
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      DestroyGameOverlayWindows();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterClasses(HINSTANCE inst) {
  WNDCLASSW host{};
  host.lpfnWndProc = HostProc;
  host.hInstance = inst;
  host.hCursor = LoadCursor(nullptr, IDC_ARROW);
  host.hIcon = g_appIcon;
  host.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  host.lpszClassName = L"Gta7In1HostV2";
  RegisterClassW(&host);

  WNDCLASSW hud{};
  hud.lpfnWndProc = gta5::app::ui::HudProc;
  hud.hInstance = inst;
  hud.hCursor = LoadCursor(nullptr, IDC_ARROW);
  hud.hIcon = g_appIcon;
  hud.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  hud.lpszClassName = L"Gta7In1HudV2";
  RegisterClassW(&hud);

  WNDCLASSW cursor{};
  cursor.lpfnWndProc = gta5::games::slider::CursorWindowProc;
  cursor.hInstance = inst;
  cursor.hCursor = LoadCursor(nullptr, IDC_ARROW);
  cursor.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  cursor.lpszClassName = L"Gta7In1CursorV2";
  RegisterClassW(&cursor);

  WNDCLASSW marks{};
  marks.lpfnWndProc = gta5::games::slider::MarksWindowProc;
  marks.hInstance = inst;
  marks.hCursor = LoadCursor(nullptr, IDC_ARROW);
  marks.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  marks.lpszClassName = L"Gta7In1MarksV2";
  RegisterClassW(&marks);

  WNDCLASSW flashing{};
  flashing.lpfnWndProc = gta5::games::flashing::OverlayWindowProc;
  flashing.hInstance = inst;
  flashing.hCursor = LoadCursor(nullptr, IDC_ARROW);
  flashing.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  flashing.lpszClassName = L"Gta7In1FlashingOverlayV2";
  RegisterClassW(&flashing);

  WNDCLASSW fingerprint{};
  fingerprint.lpfnWndProc = gta5::games::choose_fingerprint::OverlayWindowProc;
  fingerprint.hInstance = inst;
  fingerprint.hCursor = LoadCursor(nullptr, IDC_ARROW);
  fingerprint.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  fingerprint.lpszClassName = L"Gta7In1ChooseFingerprintOverlayV2";
  RegisterClassW(&fingerprint);

  WNDCLASSW sortFingerprint{};
  sortFingerprint.lpfnWndProc = gta5::games::sort_fingerprint::OverlayWindowProc;
  sortFingerprint.hInstance = inst;
  sortFingerprint.hCursor = LoadCursor(nullptr, IDC_ARROW);
  sortFingerprint.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  sortFingerprint.lpszClassName = L"Gta7In1SortFingerprintOverlayV2";
  RegisterClassW(&sortFingerprint);

  WNDCLASSW match{};
  match.lpfnWndProc = gta5::games::match::OverlayWindowProc;
  match.hInstance = inst;
  match.hCursor = LoadCursor(nullptr, IDC_ARROW);
  match.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  match.lpszClassName = L"Gta7In1MatchOverlayV1";
  RegisterClassW(&match);

  WNDCLASSW doomsdayUnlock{};
  doomsdayUnlock.lpfnWndProc = gta5::games::doomsday_unlock::OverlayWindowProc;
  doomsdayUnlock.hInstance = inst;
  doomsdayUnlock.hCursor = LoadCursor(nullptr, IDC_ARROW);
  doomsdayUnlock.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  doomsdayUnlock.lpszClassName = L"Gta7In1DoomsdayUnlockOverlayV1";
  RegisterClassW(&doomsdayUnlock);

  WNDCLASSW toast{};
  toast.lpfnWndProc = gta5::app::ui::ToastProc;
  toast.hInstance = inst;
  toast.hCursor = LoadCursor(nullptr, IDC_ARROW);
  toast.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  toast.lpszClassName = L"Gta7In1SilentToastV1";
  RegisterClassW(&toast);
}

void DestroyGameOverlayWindows() {
  HideAllGameOverlays();
  if (g_cursorOverlay) DestroyWindow(g_cursorOverlay);
  if (g_marksOverlay) DestroyWindow(g_marksOverlay);
  if (g_flashingOverlay) DestroyWindow(g_flashingOverlay);
  if (g_chooseFingerprintOverlay) DestroyWindow(g_chooseFingerprintOverlay);
  if (g_sortFingerprintOverlay) DestroyWindow(g_sortFingerprintOverlay);
  if (g_matchOverlay) DestroyWindow(g_matchOverlay);
  if (g_doomsdayUnlockOverlay) DestroyWindow(g_doomsdayUnlockOverlay);
  g_cursorOverlay = nullptr;
  g_marksOverlay = nullptr;
  g_flashingOverlay = nullptr;
  g_chooseFingerprintOverlay = nullptr;
  g_sortFingerprintOverlay = nullptr;
  g_matchOverlay = nullptr;
  g_doomsdayUnlockOverlay = nullptr;
  gta5::games::slider::SetCursorWindow(nullptr);
  gta5::games::slider::SetMarksWindow(nullptr);
  gta5::games::flashing::SetOverlayWindow(nullptr);
  gta5::games::choose_fingerprint::SetOverlayWindow(nullptr);
  gta5::games::sort_fingerprint::SetOverlayWindow(nullptr);
  gta5::games::match::SetOverlayWindow(nullptr);
  gta5::games::doomsday_unlock::SetOverlayWindow(nullptr);
}

void CreateGameOverlayWindows(HINSTANCE inst, const RECT& hudRect) {
  if (gta5::app::ui::SilentMode()) return;

  g_cursorOverlay = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      L"Gta7In1CursorV2", L"Auto Hack 7in1 Cursor", WS_POPUP,
      hudRect.right + 12, hudRect.top, gta5::games::slider::CursorSize(),
      gta5::games::slider::CursorSize(), nullptr, nullptr, inst, nullptr);
  gta5::games::slider::SetCursorWindow(g_cursorOverlay);
  if (g_cursorOverlay) {
    SetLayeredWindowAttributes(g_cursorOverlay, RGB(0, 0, 0), 255, LWA_COLORKEY);
    ShowWindow(g_cursorOverlay, SW_HIDE);
  }

  g_marksOverlay = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      L"Gta7In1MarksV2", L"Auto Hack 7in1 Marks", WS_POPUP,
      hudRect.right + 84, hudRect.top, 1, 1, nullptr, nullptr, inst, nullptr);
  gta5::games::slider::SetMarksWindow(g_marksOverlay);
  if (g_marksOverlay) {
    SetLayeredWindowAttributes(g_marksOverlay, RGB(0, 0, 0), 255, LWA_COLORKEY);
    ShowWindow(g_marksOverlay, SW_HIDE);
  }

  g_flashingOverlay = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT,
      L"Gta7In1FlashingOverlayV2", L"Auto Hack 7in1 Flashing Overlay", WS_POPUP,
      0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
      nullptr, nullptr, inst, nullptr);
  gta5::games::flashing::SetOverlayWindow(g_flashingOverlay);
  if (g_flashingOverlay) {
    SetLayeredWindowAttributes(g_flashingOverlay, RGB(0, 0, 0), 255, LWA_COLORKEY);
    ShowWindow(g_flashingOverlay, SW_HIDE);
  }

  g_chooseFingerprintOverlay = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      L"Gta7In1ChooseFingerprintOverlayV2", L"Auto Hack 7in1 Choose Fingerprint Overlay",
      WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
      nullptr, nullptr, inst, nullptr);
  gta5::games::choose_fingerprint::SetOverlayWindow(g_chooseFingerprintOverlay);
  if (g_chooseFingerprintOverlay) {
    SetLayeredWindowAttributes(g_chooseFingerprintOverlay, RGB(0, 0, 0), 255, LWA_COLORKEY);
    ShowWindow(g_chooseFingerprintOverlay, SW_HIDE);
  }

  const int virtualX = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int virtualY = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int virtualW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int virtualH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  g_sortFingerprintOverlay = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      L"Gta7In1SortFingerprintOverlayV2", L"Auto Hack 7in1 Sort Fingerprint Overlay",
      WS_POPUP, virtualX, virtualY, virtualW, virtualH, nullptr, nullptr, inst, nullptr);
  gta5::games::sort_fingerprint::SetOverlayWindow(g_sortFingerprintOverlay);
  if (g_sortFingerprintOverlay) {
    SetLayeredWindowAttributes(g_sortFingerprintOverlay, RGB(0, 0, 0), 255, LWA_COLORKEY);
    ShowWindow(g_sortFingerprintOverlay, SW_HIDE);
  }

  g_matchOverlay = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      L"Gta7In1MatchOverlayV1", L"Auto Hack 7in1 Match Overlay", WS_POPUP,
      virtualX, virtualY, virtualW, virtualH, nullptr, nullptr, inst, nullptr);
  gta5::games::match::SetOverlayWindow(g_matchOverlay);
  if (g_matchOverlay) {
    SetLayeredWindowAttributes(g_matchOverlay, RGB(0, 0, 0), 255, LWA_COLORKEY);
    ShowWindow(g_matchOverlay, SW_HIDE);
  }

  g_doomsdayUnlockOverlay = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      L"Gta7In1DoomsdayUnlockOverlayV1", L"Auto Hack 7in1 Doomsday Unlock Overlay",
      WS_POPUP, virtualX, virtualY, virtualW, virtualH, nullptr, nullptr, inst, nullptr);
  gta5::games::doomsday_unlock::SetOverlayWindow(g_doomsdayUnlockOverlay);
  if (g_doomsdayUnlockOverlay) {
    SetLayeredWindowAttributes(g_doomsdayUnlockOverlay, RGB(0, 0, 0), 255, LWA_COLORKEY);
    ShowWindow(g_doomsdayUnlockOverlay, SW_HIDE);
  }
}

void ApplyWindowMode(HINSTANCE inst) {
  HWND hud = gta5::app::ui::HudWindow();
  if (!hud) return;
  RECT hudRect{};
  GetWindowRect(hud, &hudRect);
  LONG_PTR exStyle = GetWindowLongPtrW(hud, GWL_EXSTYLE);
  if (gta5::app::ui::SilentMode()) {
    DestroyGameOverlayWindows();
    exStyle &= ~(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE);
    exStyle |= WS_EX_APPWINDOW;
    SetWindowLongPtrW(hud, GWL_EXSTYLE, exStyle);
    SetWindowPos(hud, HWND_NOTOPMOST, hudRect.left, hudRect.top,
                 gta5::app::ui::HudWidth(), gta5::app::ui::HudHeight(),
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  } else {
    exStyle |= WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_APPWINDOW;
    SetWindowLongPtrW(hud, GWL_EXSTYLE, exStyle);
    SetLayeredWindowAttributes(hud, RGB(0, 0, 0), 255, LWA_COLORKEY);
    SetWindowPos(hud, HWND_TOPMOST, hudRect.left, hudRect.top,
                 gta5::app::ui::HudWidth(), gta5::app::ui::HudHeight(),
                 SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    CreateGameOverlayWindows(inst, hudRect);
  }
  gta5::app::ui::Repaint();
}

bool CreateWindows(HINSTANCE inst) {
  g_host = CreateWindowExW(WS_EX_TOOLWINDOW, L"Gta7In1HostV2", L"Auto Hack 7in1 Host",
                           WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
  if (!g_host) return false;
  ApplyWindowIcon(g_host);
  gta5::app::ui::SetHostWindow(g_host);
  gta5::games::slider::SetHostWindow(g_host);

  RECT hudRect = gta5::app::ui::InitialHudRect();
  const DWORD hudExStyle = gta5::app::ui::SilentMode()
      ? WS_EX_APPWINDOW
      : WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_APPWINDOW;
  HWND hud = CreateWindowExW(hudExStyle,
                             L"Gta7In1HudV2", L"Auto Hack 7in1 HUD",
                             WS_POPUP, hudRect.left, hudRect.top,
                             gta5::app::ui::HudWidth(), gta5::app::ui::HudHeight(),
                             nullptr, nullptr, inst, nullptr);
  gta5::app::ui::SetHudWindow(hud);
  if (hud) {
    ApplyWindowIcon(hud);
    if (!gta5::app::ui::SilentMode()) {
      SetLayeredWindowAttributes(hud, RGB(0, 0, 0), 255, LWA_COLORKEY);
    }
    ShowWindow(hud, SW_SHOWNA);
  }
  CreateGameOverlayWindows(inst, hudRect);

  return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR commandLine, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  gta5::app::runtime::ConfigureLatencySensitiveProcess();
  g_appIcon = LoadAppIcon(inst);
  g_pluginMode = HasPluginArgument(commandLine);
  gta5::app::ui::LoadPersistentSettings();
  gta5::app::ui::SetPluginMode(g_pluginMode);

  const DWORD restartParentId = ElevatedRestartParentId(commandLine);
  if (restartParentId) {
    HANDLE restartParent = OpenProcess(SYNCHRONIZE, FALSE, restartParentId);
    if (restartParent) {
      // The old instance releases its global hotkey and mutex during shutdown.
      WaitForSingleObject(restartParent, INFINITE);
      CloseHandle(restartParent);
    }
  }

  g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\AutoHack7in1SingleInstance");
  if (!g_singleInstanceMutex) {
    gta5::app::ui::ShowNotice(inst, g_appIcon, T("notice.start_failed.title"),
                              T("notice.start_failed.message"));
    return 1;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    gta5::app::ui::ShowNotice(inst, g_appIcon, T("notice.already_running.title"),
                              T("notice.already_running.message"));
    CloseHandle(g_singleInstanceMutex);
    g_singleInstanceMutex = nullptr;
    return 0;
  }

  if (!g_pluginMode && gta5::app::ui::NeedsFirstLaunchSetup() &&
      !gta5::app::ui::RunFirstLaunchSetup(inst, g_appIcon)) {
    ReleaseMutex(g_singleInstanceMutex);
    CloseHandle(g_singleInstanceMutex);
    g_singleInstanceMutex = nullptr;
    return 0;
  }
  gta5::games::choose_fingerprint::SetUiThread();
  gta5::games::choose_fingerprint::InitStateLock();

  RegisterClasses(inst);
  if (!CreateWindows(inst)) {
    return 1;
  }

  std::wstring ready = T("log.ready");
  const size_t hotkeyToken = ready.find(L"%HOTKEY%");
  if (hotkeyToken != std::wstring::npos) {
    ready.replace(hotkeyToken, 8, gta5::app::ui::HotkeyName());
  }
  PostLog(ready);
  PostStatus(T("status.idle"));
  if (g_pluginMode) StartWorker();

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  gta5::games::choose_fingerprint::DeleteStateLock();
  if (g_singleInstanceMutex) {
    ReleaseMutex(g_singleInstanceMutex);
    CloseHandle(g_singleInstanceMutex);
    g_singleInstanceMutex = nullptr;
  }
  return 0;
}
