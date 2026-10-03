#pragma once

#include <windows.h>

#include <string>

namespace gta5::app::ui {

enum class LaunchMode {
  Resident = 0,
  Silent = 1,
};

enum class DelayPreset {
  Fast = 0,
  Slow = 1,
  Custom = 2,
};

void SetHostWindow(HWND hwnd);
void SetHudWindow(HWND hwnd);
HWND HudWindow();
void SetPluginMode(bool enabled);
bool PluginMode();
void CollapseHud();

void LoadPersistentSettings();
bool NeedsFirstLaunchSetup();
bool RunFirstLaunchSetup(HINSTANCE instance, HICON icon);
void ShowNotice(HINSTANCE instance, HICON icon, const std::wstring& title, const std::wstring& message);
bool RegisterRawKeyboardInput(HWND hwnd);
int HotkeyVk();
bool IsListeningHotkey();
void CaptureHotkeyVk(int vk);
std::wstring HotkeyName();

bool OverlayEnabled();
void SetOverlayEnabled(bool enabled);
LaunchMode CurrentLaunchMode();
bool SilentMode();
UINT ModeChangedMessage();
void ShowToggleNotification(bool enabled);
int TapHoldMs();
int TapGapMs();

void SetRunning(bool running);
void SetStatusText(const std::wstring& text);
void SetLogText(const std::wstring& text);
void Repaint();

RECT InitialHudRect();
int HudWidth();
int HudHeight();
LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

}  // namespace gta5::app::ui
