#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace gta5::input {

// Physical (per-monitor DPI aware) cursor/desktop coordinates. All mirror
// module mouse work happens in physical desktop pixels, matching the Python
// DirectGameInput backend: absolute positioning uses SendInput's 0..65535
// space mapped onto the *virtual desktop* (MOUSEEVENTF_VIRTUALDESK), so
// multi-monitor / negative-coordinate layouts keep working.
struct PhysicalPoint {
  LONG x = 0;
  LONG y = 0;
};

struct PhysicalDesktopRect {
  LONG left = 0;
  LONG top = 0;
  LONG width = 0;
  LONG height = 0;
  bool valid = false;
};

// Virtual-desktop metrics queried with Per-Monitor-v2 thread DPI awareness,
// so the values are physical pixels rather than DPI-virtualized.
PhysicalDesktopRect GetPhysicalVirtualDesktop();

// GetPhysicalCursorPos wrapper (fails when the desktop cannot be queried).
bool GetPhysicalCursorPos(PhysicalPoint* out);

// Absolute physical-cursor move, verified on arrival (Python semantics:
// CURSOR_POSITION_MISMATCH when the cursor lands more than tolerancePx away).
bool MoveCursorAbsolute(LONG screenX, LONG screenY,
                        PhysicalPoint* actualOut = nullptr, int tolerancePx = 2);

bool SetLeftButton(bool down);

// Left click at the current cursor position (down, holdMs, up).
bool ClickLeftHere(int holdMs = 35, PhysicalPoint* clickedAt = nullptr);

// Left click after an absolute move; releases nothing — the caller owns the
// key-hold state (Python's click/move paths key_up() first).
bool ClickLeftAt(LONG screenX, LONG screenY, int holdMs = 35,
                 PhysicalPoint* clickedAt = nullptr);

}  // namespace gta5::input
