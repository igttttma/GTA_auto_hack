#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include "../capture/game_window.h"

namespace gta5::games::slider {
void SetHostWindow(HWND hwnd);
void SetCursorWindow(HWND hwnd);
void SetMarksWindow(HWND hwnd);
HWND CursorWindow();
HWND MarksWindow();
void ClearOverlayState();
void HideTransientOverlays();
bool DetectInGame(const gta5::capture::GameFrame& frame);
void ResetInGameCache();
void RunSession(const std::function<bool()>& stopRequested);
int CursorSize();
LRESULT CALLBACK CursorWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK MarksWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
}  // namespace gta5::games::slider

namespace gta5::games::flashing {
bool DetectInGame(const gta5::capture::GameFrame& frame);
void ResetInGameCache();
HWND OverlayWindow();
void SetOverlayWindow(HWND hwnd);
void HideOverlay();
bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<bool()>& overlayEnabled,
                const std::function<void(const std::wstring&)>& status);
LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
}  // namespace gta5::games::flashing

namespace gta5::games::choose_fingerprint {
bool DetectInGame(const gta5::capture::GameFrame& frame);
void ResetInGameCache();
HWND OverlayWindow();
void SetOverlayWindow(HWND hwnd);
void SetUiThread();
void InitStateLock();
void DeleteStateLock();
void ClearOverlay();
bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<bool()>& overlayEnabled,
                const std::function<void(const std::wstring&)>& status);
LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
}  // namespace gta5::games::choose_fingerprint

namespace gta5::games::sort_fingerprint {
bool DetectInGame(const gta5::capture::GameFrame& frame);
void ResetInGameCache();
void SetOverlayWindow(HWND hwnd);
void ClearOverlay();
bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<bool()>& overlayEnabled,
                const std::function<void(const std::wstring&)>& status,
                const std::function<void(const std::wstring&)>& log);
LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
}  // namespace gta5::games::sort_fingerprint

namespace gta5::games::fleeca {
bool DetectInGame(const gta5::capture::GameFrame& frame);
void ResetInGameCache();
bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<void(const std::wstring&)>& status);
}  // namespace gta5::games::fleeca

namespace gta5::games::find_number {
bool DetectInGame(const gta5::capture::GameFrame& frame);
void ResetInGameCache();
bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<void(const std::wstring&)>& status);
}  // namespace gta5::games::find_number

namespace gta5::games::match {
bool DetectInGame(const gta5::capture::GameFrame& frame);
void ResetInGameCache();
void SetOverlayWindow(HWND hwnd);
void ClearOverlay();
bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<bool()>& overlayEnabled,
                const std::function<void(const std::wstring&)>& status);
LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
}  // namespace gta5::games::match

namespace gta5::games::doomsday_unlock {
bool DetectInGame(const gta5::capture::GameFrame& frame);
void ResetInGameCache();
bool RunSession(const std::function<bool()>& stopRequested,
                const std::function<bool()>& overlayEnabled,
                const std::function<void(const std::wstring&)>& status);
void SetOverlayWindow(HWND hwnd);
void ClearOverlay();
LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
}  // namespace gta5::games::doomsday_unlock
