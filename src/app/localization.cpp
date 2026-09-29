#include "localization.h"

#include "../resources/resource.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string>
#include <unordered_map>

namespace gta5::app::l10n {
namespace {

std::atomic<int> g_language{static_cast<int>(Language::Chinese)};

const std::unordered_map<std::string, UINT>& StringIds() {
  static const std::unordered_map<std::string, UINT> ids = {
      {"state.on", IDS_STATE_ON},
      {"state.off", IDS_STATE_OFF},
      {"panel.mode", IDS_PANEL_MODE},
      {"mode.resident", IDS_MODE_RESIDENT},
      {"mode.silent", IDS_MODE_SILENT},
      {"panel.hotkey", IDS_PANEL_HOTKEY},
      {"action.confirm", IDS_ACTION_CONFIRM},
      {"action.change", IDS_ACTION_CHANGE},
      {"admin.try_prompt", IDS_ADMIN_TRY_PROMPT},
      {"admin.run", IDS_ADMIN_RUN},
      {"panel.capture", IDS_PANEL_CAPTURE},
      {"capture.gdi", IDS_CAPTURE_GDI},
      {"capture.dxgi", IDS_CAPTURE_DXGI},
      {"hint.capture_gdi", IDS_HINT_CAPTURE_GDI},
      {"hint.capture_dxgi", IDS_HINT_CAPTURE_DXGI},
      {"setup.capture", IDS_SETUP_CAPTURE},
      {"setup.capture_desc", IDS_SETUP_CAPTURE_DESC},
      {"capture.window_mode", IDS_CAPTURE_WINDOW_MODE},
      {"capture.display_mode", IDS_CAPTURE_DISPLAY_MODE},
      {"panel.key_delay", IDS_PANEL_KEY_DELAY},
      {"panel.language", IDS_PANEL_LANGUAGE},
      {"delay.fast", IDS_DELAY_FAST},
      {"delay.slow", IDS_DELAY_SLOW},
      {"delay.custom", IDS_DELAY_CUSTOM},
      {"delay.fast_values", IDS_DELAY_FAST_VALUES},
      {"delay.slow_values", IDS_DELAY_SLOW_VALUES},
      {"delay.hold", IDS_DELAY_HOLD},
      {"delay.gap", IDS_DELAY_GAP},
      {"unit.ms", IDS_UNIT_MS},
      {"overlay.show", IDS_OVERLAY_SHOW},
      {"overlay.silent", IDS_OVERLAY_SILENT},
      {"language.chinese", IDS_LANGUAGE_CHINESE},
      {"language.english", IDS_LANGUAGE_ENGLISH},
      {"hint.resident", IDS_HINT_RESIDENT},
      {"hint.silent", IDS_HINT_SILENT},
      {"hint.hotkey", IDS_HINT_HOTKEY},
      {"hint.fast", IDS_HINT_FAST},
      {"hint.slow", IDS_HINT_SLOW},
      {"hint.custom", IDS_HINT_CUSTOM},
      {"hint.overlay", IDS_HINT_OVERLAY},
      {"hint.language", IDS_HINT_LANGUAGE},
      {"validation.delay", IDS_VALIDATION_DELAY},
      {"setup.title", IDS_SETUP_TITLE},
      {"setup.startup_mode", IDS_SETUP_STARTUP_MODE},
      {"setup.resident_desc", IDS_SETUP_RESIDENT_DESC},
      {"setup.silent_desc", IDS_SETUP_SILENT_DESC},
      {"setup.default_delay", IDS_SETUP_DEFAULT_DELAY},
      {"setup.delay_desc", IDS_SETUP_DELAY_DESC},
      {"setup.duration", IDS_SETUP_DURATION},
      {"setup.interval", IDS_SETUP_INTERVAL},
      {"action.cancel", IDS_ACTION_CANCEL},
      {"action.start", IDS_ACTION_START},
      {"action.ok", IDS_ACTION_OK},
      {"notice.subtitle", IDS_NOTICE_SUBTITLE},
      {"toast.on", IDS_TOAST_ON},
      {"toast.off", IDS_TOAST_OFF},
      {"status.idle", IDS_STATUS_IDLE},
      {"status.starting", IDS_STATUS_STARTING},
      {"status.stopping", IDS_STATUS_STOPPING},
      {"status.stopped", IDS_STATUS_STOPPED},
      {"status.search_game", IDS_STATUS_SEARCH_GAME},
      {"status.search_minigame", IDS_STATUS_SEARCH_MINIGAME},
      {"status.running", IDS_STATUS_RUNNING},
      {"status.detect_timeout", IDS_STATUS_DETECT_TIMEOUT},
      {"status.game_timeout", IDS_STATUS_GAME_TIMEOUT},
      {"status.completed", IDS_STATUS_COMPLETED},
      {"status.slider_latency", IDS_STATUS_SLIDER_LATENCY},
      {"status.capture_failed", IDS_STATUS_CAPTURE_FAILED},
      {"status.slider_timeout", IDS_STATUS_SLIDER_TIMEOUT},
      {"status.waiting_yellow", IDS_STATUS_WAITING_YELLOW},
      {"status.in_minigame", IDS_STATUS_IN_MINIGAME},
      {"status.fingerprint_locating", IDS_STATUS_FINGERPRINT_LOCATING},
      {"status.fingerprint_exited", IDS_STATUS_FINGERPRINT_EXITED},
      {"status.fingerprint_confirm_exit", IDS_STATUS_FINGERPRINT_CONFIRM_EXIT},
      {"status.fingerprint_complete", IDS_STATUS_FINGERPRINT_COMPLETE},
      {"status.fingerprint_input", IDS_STATUS_FINGERPRINT_INPUT},
      {"status.fingerprint_next", IDS_STATUS_FINGERPRINT_NEXT},
      {"status.sort_locating", IDS_STATUS_SORT_LOCATING},
      {"status.sort_analyzing", IDS_STATUS_SORT_ANALYZING},
      {"status.sort_next", IDS_STATUS_SORT_NEXT},
      {"status.sort_analyzing_next", IDS_STATUS_SORT_ANALYZING_NEXT},
      {"status.sort_complete", IDS_STATUS_SORT_COMPLETE},
      {"status.sort_verifying", IDS_STATUS_SORT_VERIFYING},
      {"status.flashing_locating", IDS_STATUS_FLASHING_LOCATING},
      {"status.flashing_next_level", IDS_STATUS_FLASHING_NEXT_LEVEL},
      {"status.flashing_exited", IDS_STATUS_FLASHING_EXITED},
      {"status.flashing_confirm_exit", IDS_STATUS_FLASHING_CONFIRM_EXIT},
      {"status.flashing_wait_next", IDS_STATUS_FLASHING_WAIT_NEXT},
      {"status.flashing_verify", IDS_STATUS_FLASHING_VERIFY},
      {"status.flashing_read", IDS_STATUS_FLASHING_READ},
      {"status.flashing_input", IDS_STATUS_FLASHING_INPUT},
      {"status.fleeca_planning", IDS_STATUS_FLEECA_PLANNING},
      {"status.fleeca_starting", IDS_STATUS_FLEECA_STARTING},
      {"status.fleeca_navigating", IDS_STATUS_FLEECA_NAVIGATING},
      {"status.fleeca_wait_result", IDS_STATUS_FLEECA_WAIT_RESULT},
      {"status.fleeca_detect_result", IDS_STATUS_FLEECA_DETECT_RESULT},
      {"status.fleeca_result_delay", IDS_STATUS_FLEECA_RESULT_DELAY},
      {"status.fleeca_advancing", IDS_STATUS_FLEECA_ADVANCING},
      {"status.fleeca_wait_next", IDS_STATUS_FLEECA_WAIT_NEXT},
      {"status.fleeca_next", IDS_STATUS_FLEECA_NEXT},
      {"status.fleeca_completed", IDS_STATUS_FLEECA_COMPLETED},
      {"status.fleeca_route_failed", IDS_STATUS_FLEECA_ROUTE_FAILED},
      {"status.fleeca_signal_failed", IDS_STATUS_FLEECA_SIGNAL_FAILED},
      {"status.fleeca_direction_failed", IDS_STATUS_FLEECA_DIRECTION_FAILED},
      {"status.fleeca_result_failed", IDS_STATUS_FLEECA_RESULT_FAILED},
      {"status.find_number_locating", IDS_STATUS_FIND_NUMBER_LOCATING},
      {"status.find_number_analyzing", IDS_STATUS_FIND_NUMBER_ANALYZING},
      {"status.find_number_moving", IDS_STATUS_FIND_NUMBER_MOVING},
      {"status.find_number_verifying", IDS_STATUS_FIND_NUMBER_VERIFYING},
      {"status.find_number_submitting", IDS_STATUS_FIND_NUMBER_SUBMITTING},
      {"status.find_number_complete", IDS_STATUS_FIND_NUMBER_COMPLETE},
      {"status.find_number_exited", IDS_STATUS_FIND_NUMBER_EXITED},
      {"status.match_locating", IDS_STATUS_MATCH_LOCATING},
      {"status.match_analyzing", IDS_STATUS_MATCH_ANALYZING},
      {"status.match_reading", IDS_STATUS_MATCH_READING},
      {"status.match_connecting", IDS_STATUS_MATCH_CONNECTING},
      {"status.match_verifying", IDS_STATUS_MATCH_VERIFYING},
      {"status.match_complete", IDS_STATUS_MATCH_COMPLETE},
      {"status.match_exited", IDS_STATUS_MATCH_EXITED},
      {"status.match_no_solution", IDS_STATUS_MATCH_NO_SOLUTION},
      {"overlay.clicks", IDS_OVERLAY_CLICKS},
      {"notice.start_failed.title", IDS_NOTICE_START_FAILED_TITLE},
      {"notice.start_failed.message", IDS_NOTICE_START_FAILED_MESSAGE},
      {"notice.already_running.title", IDS_NOTICE_ALREADY_RUNNING_TITLE},
      {"notice.already_running.message", IDS_NOTICE_ALREADY_RUNNING_MESSAGE},
      {"log.start", IDS_LOG_START},
      {"log.found_game", IDS_LOG_FOUND_GAME},
      {"log.ready", IDS_LOG_READY},
      {"game.slider", IDS_GAME_SLIDER},
      {"game.flashing", IDS_GAME_FLASHING},
      {"game.choose_fingerprint", IDS_GAME_CHOOSE_FINGERPRINT},
      {"game.sort_fingerprint", IDS_GAME_SORT_FINGERPRINT},
      {"game.fleeca", IDS_GAME_FLEECA},
      {"game.find_number", IDS_GAME_FIND_NUMBER},
      {"game.match", IDS_GAME_MATCH},
      {"game.doomsday_unlock", IDS_GAME_DOOMSDAY_UNLOCK},
      {"game.none", IDS_GAME_NONE},
  };
  return ids;
}

LANGID ResourceLanguage(Language language) {
  return language == Language::Chinese
             ? MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)
             : MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
}

std::wstring LoadLocalizedString(UINT id, Language language) {
  HMODULE module = GetModuleHandleW(nullptr);
  const UINT blockId = (id >> 4) + 1;
  HRSRC resource = FindResourceExW(module, RT_STRING, MAKEINTRESOURCEW(blockId),
                                   ResourceLanguage(language));
  if (!resource) return {};
  HGLOBAL loaded = LoadResource(module, resource);
  const WORD* cursor = loaded ? static_cast<const WORD*>(LockResource(loaded)) : nullptr;
  if (!cursor) return {};
  for (UINT index = 0; index < (id & 15); ++index) cursor += 1 + *cursor;
  const WORD length = *cursor++;
  return length ? std::wstring(reinterpret_cast<const wchar_t*>(cursor), length) : std::wstring();
}

}  // namespace

void SetLanguage(Language language) {
  g_language.store(static_cast<int>(language), std::memory_order_relaxed);
}

Language CurrentLanguage() {
  return static_cast<Language>(g_language.load(std::memory_order_relaxed));
}

std::wstring Text(const char* key) {
  const auto found = StringIds().find(key);
  if (found == StringIds().end()) return std::wstring(key, key + std::strlen(key));
  std::wstring value = LoadLocalizedString(found->second, CurrentLanguage());
  if (value.empty() && CurrentLanguage() != Language::English) {
    value = LoadLocalizedString(found->second, Language::English);
  }
  return value.empty() ? std::wstring(key, key + std::strlen(key)) : value;
}

}  // namespace gta5::app::l10n
