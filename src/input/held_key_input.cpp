#include "held_key_input.h"

#include <atomic>
#include <map>
#include <utility>

namespace gta5::input {
namespace {

constexpr DWORD kSpiGetKeyboardDelay = 0x0016;  // 0..3
constexpr DWORD kSpiGetKeyboardSpeed = 0x000A;  // 0..31

bool IsSupportedControlKey(WORD virtualKey) {
  return virtualKey == HeldKeyInput::kKeyA ||
         virtualKey == HeldKeyInput::kKeyD ||
         virtualKey == HeldKeyInput::kKeyTab;
}

bool SendKeyScan(WORD virtualKey, bool up) {
  const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
  if (!scanCode) return false;
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wScan = static_cast<WORD>(scanCode);
  input.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
  return SendInput(1, &input, sizeof(input)) == 1;
}

}  // namespace

HeldKeyInput::TypematicProfile HeldKeyInput::QueryTypematicProfile() {
  // This machine's actual keyboard repeat delay/rate (SPI), not guesses.
  UINT delay = 1;
  UINT speed = 31;
  SystemParametersInfoW(kSpiGetKeyboardDelay, 0, &delay, 0);
  SystemParametersInfoW(kSpiGetKeyboardSpeed, 0, &speed, 0);
  const double delayS = (250.0 + 250.0 * delay) / 1000.0;
  const double periodS = std::max(0.024, (400.0 - 12.0 * speed) / 1000.0);
  return {std::chrono::microseconds(static_cast<long long>(delayS * 1e6)),
          std::chrono::microseconds(static_cast<long long>(periodS * 1e6))};
}

HeldKeyInput::HeldKeyInput() : profile_(QueryTypematicProfile()) {}

HeldKeyInput::~HeldKeyInput() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pumpStop_ = true;
  }
  if (pumpThread_.joinable()) pumpThread_.join();
}

bool HeldKeyInput::KeyDown(WORD virtualKey) {
  if (!IsSupportedControlKey(virtualKey)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  pulsePending_ = false;  // an explicit key change cancels a timed release
  if (heldKey_ == virtualKey) return true;
  const WORD previous = std::exchange(heldKey_, 0);
  if (previous) SendKeyScan(previous, true);
  if (!SendKeyScan(virtualKey, false)) return false;
  heldKey_ = virtualKey;
  StartPumpLocked();
  return true;
}

void HeldKeyInput::KeyUp() {
  std::lock_guard<std::mutex> lock(mutex_);
  pulsePending_ = false;
  const WORD vk = std::exchange(heldKey_, 0);
  if (vk) SendKeyScan(vk, true);
  const WORD turbo = std::exchange(turboKey_, 0);
  if (turbo) SendKeyScan(turbo, true);
}

void HeldKeyInput::PulseTimed(WORD virtualKey, double seconds) {
  KeyDown(virtualKey);
  std::lock_guard<std::mutex> lock(mutex_);
  pulseReleaseAt_ =
      Clock::now() + std::chrono::duration_cast<Clock::duration>(
                         std::chrono::duration<double>(seconds));
  pulsePending_ = true;
  StartPumpLocked();
}

bool HeldKeyInput::SetTurbo(WORD virtualKey) {
  if (virtualKey != 0 && !IsSupportedControlKey(virtualKey)) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  if (turboKey_ == virtualKey) return false;
  const WORD turbo = std::exchange(turboKey_, 0);
  if (turbo) SendKeyScan(turbo, true);
  if (virtualKey == 0) return false;
  if (!SendKeyScan(virtualKey, false)) return false;
  turboKey_ = virtualKey;
  StartPumpLocked();
  return true;
}

void HeldKeyInput::ReleaseAll() {
  KeyUp();
  SendKeyScan(kKeyA, true);
  SendKeyScan(kKeyD, true);
  SendKeyScan(kKeyTab, true);
}

bool HeldKeyInput::ClickHere(int holdMs, PhysicalPoint* clickedAt) {
  KeyUp();
  return ClickLeftHere(holdMs, clickedAt);
}

bool HeldKeyInput::MoveCursorTo(LONG screenX, LONG screenY,
                                PhysicalPoint* actualOut) {
  KeyUp();
  return MoveCursorAbsolute(screenX, screenY, actualOut);
}

bool HeldKeyInput::ClickAt(LONG screenX, LONG screenY, int holdMs,
                           PhysicalPoint* clickedAt) {
  KeyUp();
  return ClickLeftAt(screenX, screenY, holdMs, clickedAt);
}

bool HeldKeyInput::turboHeld() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return turboKey_ != 0;
}

void HeldKeyInput::StartPumpLocked() {
  if (pumpThread_.joinable()) return;
  pumpStop_ = false;
  pumpThread_ = std::thread([this] {
    // Re-send held keys' keyDown — silence for the typematic delay, then the
    // typematic rate, all under the key lock: a repeat that lost the race
    // after a key_up would leave the key stuck down. Only the turbo key
    // repeats; the direction key never does.
    std::map<WORD, Clock::time_point> nextFire;
    for (;;) {
      Sleep(2);
      std::lock_guard<std::mutex> lock(mutex_);
      if (pumpStop_) return;
      const WORD held = turboKey_;
      const Clock::time_point now = Clock::now();
      if (pulsePending_ && now >= pulseReleaseAt_) {
        pulsePending_ = false;
        const WORD vk = std::exchange(heldKey_, 0);
        if (vk) SendKeyScan(vk, true);
        const WORD turbo = std::exchange(turboKey_, 0);
        if (turbo) SendKeyScan(turbo, true);
      }
      for (auto it = nextFire.begin(); it != nextFire.end();) {
        if (it->first != held) it = nextFire.erase(it);
        else ++it;
      }
      if (!held) continue;
      auto it = nextFire.find(held);
      if (it == nextFire.end()) {
        nextFire.emplace(held, now + profile_.repeatDelay);
      } else if (it->second <= now) {
        SendKeyScan(held, false);
        it->second = now + profile_.repeatPeriod;
      }
    }
  });
}

}  // namespace gta5::input
