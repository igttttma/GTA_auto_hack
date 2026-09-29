#include "mouse_input.h"

#include <cmath>
#include <utility>

namespace gta5::input {
namespace {

constexpr UINT kMouseAbsoluteFlags =
    MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
constexpr int kSmXVirtualScreen = 76;
constexpr int kSmYVirtualScreen = 77;
constexpr int kSmCxVirtualScreen = 78;
constexpr int kSmCyVirtualScreen = 79;
DPI_AWARENESS_CONTEXT SetThreadDpiContext(DPI_AWARENESS_CONTEXT context) {
  using Fn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
  static const Fn fn = []() -> Fn {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32
        ? reinterpret_cast<Fn>(reinterpret_cast<void*>(
              GetProcAddress(user32, "SetThreadDpiAwarenessContext")))
        : nullptr;
  }();
  return fn ? fn(context) : nullptr;
}

LONG NormalizeAxis(LONG value, LONG origin, LONG extent) {
  if (extent <= 1) return 0;
  LONG relative = value - origin;
  LONG scaled = static_cast<LONG>(
      std::llround(static_cast<double>(relative) * 65535.0 /
                   static_cast<double>(extent - 1)));
  if (scaled < 0) scaled = 0;
  if (scaled > 65535) scaled = 65535;
  return scaled;
}

}  // namespace

PhysicalDesktopRect GetPhysicalVirtualDesktop() {
  PhysicalDesktopRect rect;
  DPI_AWARENESS_CONTEXT previous =
      SetThreadDpiContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  rect.left = GetSystemMetrics(kSmXVirtualScreen);
  rect.top = GetSystemMetrics(kSmYVirtualScreen);
  rect.width = GetSystemMetrics(kSmCxVirtualScreen);
  rect.height = GetSystemMetrics(kSmCyVirtualScreen);
  if (previous) SetThreadDpiContext(previous);
  rect.valid = rect.width > 0 && rect.height > 0;
  return rect;
}

bool GetPhysicalCursorPos(PhysicalPoint* out) {
  if (!out) return false;
  POINT point{};
  using Fn = BOOL(WINAPI*)(LPPOINT);
  static const Fn fn = []() -> Fn {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    return user32
        ? reinterpret_cast<Fn>(reinterpret_cast<void*>(
              GetProcAddress(user32, "GetPhysicalCursorPos")))
        : nullptr;
  }();
  if (!fn || !fn(&point)) return false;
  out->x = point.x;
  out->y = point.y;
  return true;
}

bool MoveCursorAbsolute(LONG screenX, LONG screenY, PhysicalPoint* actualOut,
                        int tolerancePx) {
  const PhysicalDesktopRect desktop = GetPhysicalVirtualDesktop();
  if (!desktop.valid) return false;
  if (screenX < desktop.left || screenX >= desktop.left + desktop.width ||
      screenY < desktop.top || screenY >= desktop.top + desktop.height) {
    return false;  // CURSOR_TARGET_OUTSIDE_DESKTOP
  }
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dx = NormalizeAxis(screenX, desktop.left, desktop.width);
  input.mi.dy = NormalizeAxis(screenY, desktop.top, desktop.height);
  input.mi.dwFlags = kMouseAbsoluteFlags;
  if (SendInput(1, &input, sizeof(input)) != 1) return false;
  PhysicalPoint moved{};
  if (!GetPhysicalCursorPos(&moved)) return false;
  if (actualOut) *actualOut = moved;
  const LONG dx = moved.x > screenX ? moved.x - screenX : screenX - moved.x;
  const LONG dy = moved.y > screenY ? moved.y - screenY : screenY - moved.y;
  return dx <= tolerancePx && dy <= tolerancePx;
}

bool SetLeftButton(bool down) {
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
  return SendInput(1, &input, sizeof(input)) == 1;
}

bool ClickLeftHere(int holdMs, PhysicalPoint* clickedAt) {
  PhysicalPoint position{};
  if (!GetPhysicalCursorPos(&position)) return false;
  if (clickedAt) *clickedAt = position;
  if (!SetLeftButton(true)) return false;
  Sleep(holdMs < 0 ? 0 : static_cast<DWORD>(holdMs));
  return SetLeftButton(false);
}

bool ClickLeftAt(LONG screenX, LONG screenY, int holdMs,
                 PhysicalPoint* clickedAt) {
  PhysicalPoint moved{};
  if (!MoveCursorAbsolute(screenX, screenY, &moved)) return false;
  return ClickLeftHere(holdMs, clickedAt);
}

}  // namespace gta5::input
