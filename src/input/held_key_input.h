#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <mutex>
#include <thread>

#include "mouse_input.h"

namespace gta5::input {

// Port of the Python DirectGameInput model (ground truth from the key
// capture: a manual rotation hold is "TAB repeats at typematic cadence while
// the direction key stays a SINGLE keyDown"). Exactly one direction key is
// held at a time; the turbo key (TAB) is held alongside it and re-sent by one
// pump thread — silence for the typematic delay, then the typematic rate.
// Only A / D / TAB are valid control keys, matching the Python side.
class HeldKeyInput {
 public:
  static constexpr WORD kKeyA = 0x41;
  static constexpr WORD kKeyD = 0x44;
  static constexpr WORD kKeyTab = 0x09;

  HeldKeyInput();
  ~HeldKeyInput();

  HeldKeyInput(const HeldKeyInput&) = delete;
  HeldKeyInput& operator=(const HeldKeyInput&) = delete;

  // Holds the direction key (releasing any previously held one first).
  bool KeyDown(WORD virtualKey);
  // Releases the held pair (direction + turbo). Every release path ends both.
  void KeyUp();
  // Key down now, key up `seconds` later, timed by the input layer's 2 ms
  // pump thread — not by the 30 Hz observer tick.  A short tap is the
  // game's single-step actuator and its width lives near one frame period,
  // where tick-quantized releases would be bimodal.  Any explicit
  // KeyDown/KeyUp cancels a pending timed release.
  void PulseTimed(WORD virtualKey, double seconds);
  // Holds the turbo key, or releases it when virtualKey is 0.
  // Returns true when this call pressed it fresh.
  bool SetTurbo(WORD virtualKey);
  void Pulse(WORD virtualKey) { KeyDown(virtualKey); }
  // Releases the pair and force-releases A/D/TAB even if tracking was lost.
  void ReleaseAll();

  // Same composition as Python DirectGameInput: mouse operations first
  // release the held pair.
  bool ClickHere(int holdMs = 35, PhysicalPoint* clickedAt = nullptr);
  bool MoveCursorTo(LONG screenX, LONG screenY,
                    PhysicalPoint* actualOut = nullptr);
  bool ClickAt(LONG screenX, LONG screenY, int holdMs = 35,
               PhysicalPoint* clickedAt = nullptr);

  bool turboHeld() const;

 private:
  using Clock = std::chrono::steady_clock;

  struct TypematicProfile {
    std::chrono::microseconds repeatDelay;
    std::chrono::microseconds repeatPeriod;
  };

  static TypematicProfile QueryTypematicProfile();
  void StartPumpLocked();

  mutable std::mutex mutex_;
  WORD heldKey_ = 0;
  WORD turboKey_ = 0;
  TypematicProfile profile_;
  std::thread pumpThread_;
  bool pumpStop_ = false;
  bool pulsePending_ = false;  // guarded by mutex_
  Clock::time_point pulseReleaseAt_{};
};

}  // namespace gta5::input
