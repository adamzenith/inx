/**
 * @file ReaderButtonBindings.cpp
 * @brief Definitions for ReaderButtonBindings.
 */

#include "ReaderButtonBindings.h"

#include <Arduino.h>
#include <GfxRenderer.h>

#include "EpubActivity.h"
#include "state/ReaderSetting.h"
#include "state/SystemSetting.h"

bool ReaderButtonBindings::handleInput(EpubActivity& act) {
  // Stops at the first button whose action fires this frame - if one dispatch enters a different UI
  // mode (dictionary/annotation/menu/settings), the remaining buttons shouldn't also dispatch into it
  // on the same frame. A one-frame delay in starting to track a later button's press is imperceptible
  // against the ~700ms long-press threshold.
  if (handleButton(act, MappedInputManager::Button::Up, upState_, READER_SETTINGS.btnUpShortAction,
                   READER_SETTINGS.btnUpLongAction)) {
    return true;
  }
  if (handleButton(act, MappedInputManager::Button::Down, downState_, READER_SETTINGS.btnDownShortAction,
                   READER_SETTINGS.btnDownLongAction)) {
    return true;
  }
  if (handleButton(act, MappedInputManager::Button::Left, leftState_, READER_SETTINGS.btnLeftShortAction,
                   READER_SETTINGS.btnLeftLongAction)) {
    return true;
  }
  if (handleButton(act, MappedInputManager::Button::Right, rightState_, READER_SETTINGS.btnRightShortAction,
                   READER_SETTINGS.btnRightLongAction)) {
    return true;
  }
  return false;
}

bool ReaderButtonBindings::handleButton(EpubActivity& act, const MappedInputManager::Button button,
                                        PressState& state, const uint8_t shortAction, const uint8_t longAction) {
  // Latched edges: a press+release pair that both landed since the last input update (e.g. a fast
  // tap during a panel refresh) arrives with pressedEdge && releasedEdge && !held and still fires.
  const bool pressedEdge = act.mappedInput.wasPressed(button);
  const bool releasedEdge = act.mappedInput.wasReleased(button);
  const bool held = act.mappedInput.isPressed(button);
  const unsigned long now = millis();

  if (pressedEdge && !state.active) {
    state.active = true;
    state.pressStartMs = now;
    state.longFired = false;
    state.shortFired = false;
    // No long action bound: nothing to disambiguate, so fire on the press edge and save the
    // hold-duration latency. Suppressed while a dictionary/annotation chord is forming so the
    // chord's Left/Right doesn't also turn a page.
    if (longAction == SystemSetting::BTN_ACTION_NONE && !act.anyChordPending()) {
      state.shortFired = true;
      dispatchShort(act, button, shortAction);
      return true;
    }
  }

  if (state.active && !state.longFired && !state.shortFired && held && !releasedEdge &&
      now - state.pressStartMs >= kLongPressMs) {
    state.longFired = true;
    dispatch(act, longAction);
    return true;
  }

  if (state.active && (releasedEdge || !held)) {
    state.active = false;
    if (state.longFired || state.shortFired) {
      return false;  // already fired on this press - don't also fire short on release
    }
    dispatchShort(act, button, shortAction);
    return true;
  }

  return false;
}

void ReaderButtonBindings::dispatchShort(EpubActivity& act, const MappedInputManager::Button button,
                                         const uint8_t shortAction) {
  uint8_t repeat = 1;
  if (shortAction == SystemSetting::BTN_ACTION_PAGE_NEXT || shortAction == SystemSetting::BTN_ACTION_PAGE_PREVIOUS) {
    repeat = act.mappedInput.takePressCount(button);
    if (repeat == 0) repeat = 1;
    if (repeat > kMaxCoalescedTurns) repeat = kMaxCoalescedTurns;
  }
  dispatch(act, shortAction, repeat);
}

void ReaderButtonBindings::dispatch(EpubActivity& act, const uint8_t action, const uint8_t repeat) {
  switch (action) {
    case SystemSetting::BTN_ACTION_NONE:
      break;
    case SystemSetting::BTN_ACTION_PAGE_NEXT:
    case SystemSetting::BTN_ACTION_PAGE_PREVIOUS: {
      const bool forward = action == SystemSetting::BTN_ACTION_PAGE_NEXT;
      act.endPageTimer();
      // pageTurn() is pure index math (no I/O) - N coalesced presses collapse into one render.
      for (uint8_t i = 0; i < repeat; i++) {
        act.pageTurn(forward);
      }
      if (repeat > 1 || act.renderer.isRefreshInFlight()) {
        act.rapidTurn_ = true;
      }
      act.lastAutoPageTurnTime = millis();
      break;
    }
    case SystemSetting::BTN_ACTION_OPEN_SETTINGS:
      act.pauseReadingStats();
      act.toggleSettingsDrawer();
      break;
    case SystemSetting::BTN_ACTION_TABLE_OF_CONTENTS:
      act.openTableOfContents();
      break;
    case SystemSetting::BTN_ACTION_CHANGE_ORIENTATION:
      act.orientationPicker_.enter(act);
      break;
    case SystemSetting::BTN_ACTION_ANNOTATE:
      act.pauseReadingStats();
      act.annUi_.enter(act);
      break;
    case SystemSetting::BTN_ACTION_DICTIONARY:
      act.pauseReadingStats();
      act.dictUi_.enter(act);
      break;
    case SystemSetting::BTN_ACTION_PAGE_REFRESH:
      act.renderer.displayBuffer(HalDisplay::MANUAL_REFRESH);
      act.updateRequired = true;
      break;
    case SystemSetting::BTN_ACTION_CHAPTER_SKIP_NEXT:
    case SystemSetting::BTN_ACTION_CHAPTER_SKIP_PREVIOUS: {
      const bool forward = action == SystemSetting::BTN_ACTION_CHAPTER_SKIP_NEXT;
      act.endPageTimer();
      bool spineAdvanced = false;
      if (forward) {
        if (act.currentSpineIndex < act.epub->getSpineItemsCount() - 1) {
          act.currentSpineIndex++;
          act.nextPageNumber = 0;
          act.section.reset();
          spineAdvanced = true;
        }
      } else if (act.currentSpineIndex > 0) {
        act.currentSpineIndex--;
        act.nextPageNumber = 0;
        act.section.reset();
        spineAdvanced = true;
      }
      if (spineAdvanced) {
        act.startPageTimer();
        act.lastAutoPageTurnTime = millis();
        act.updateRequired = true;
      }
      break;
    }
    case SystemSetting::BTN_ACTION_BOOKMARK:
      act.pauseReadingStats();
      act.addBookmark();
      act.startPageTimer();
      break;
    default:
      break;
  }
}
