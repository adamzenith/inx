/**
 * @file InputManager.cpp
 * @brief Definitions for InputManager.
 */

#include "InputManager.h"

const int InputManager::ADC_RANGES_1[] = {ADC_NO_BUTTON, 3100, 2090, 750, INT32_MIN};
const int InputManager::ADC_RANGES_2[] = {ADC_NO_BUTTON, 1120, INT32_MIN};
const char* InputManager::BUTTON_NAMES[] = {"Back", "Confirm", "Left", "Right", "Up", "Down", "Power"};

InputManager::InputManager()
    : currentState(0),
      lastState(0),
      pressedEvents(0),
      releasedEvents(0),
      lastDebounceTime(0),
      buttonPressStart(0),
      buttonPressFinish(0) {}

void InputManager::begin() {
  pinMode(BUTTON_ADC_PIN_1, INPUT);
  pinMode(BUTTON_ADC_PIN_2, INPUT);
  pinMode(POWER_BUTTON_PIN, INPUT_PULLUP);
  analogSetAttenuation(ADC_11db);
}

int InputManager::getButtonFromADC(const int adcValue, const int ranges[], const int numButtons) {
  for (int i = 0; i < numButtons; i++) {
    if (ranges[i + 1] < adcValue && adcValue <= ranges[i]) {
      return i;
    }
  }

  return -1;
}

uint8_t InputManager::getState() {
  uint8_t state = 0;

  const int adcValue1 = analogRead(BUTTON_ADC_PIN_1);
  const int button1 = getButtonFromADC(adcValue1, ADC_RANGES_1, NUM_BUTTONS_1);
  if (button1 >= 0) {
    state |= (1 << button1);
  }

  const int adcValue2 = analogRead(BUTTON_ADC_PIN_2);
  const int button2 = getButtonFromADC(adcValue2, ADC_RANGES_2, NUM_BUTTONS_2);
  if (button2 >= 0) {
    state |= (1 << (button2 + 4));
  }

  if (digitalRead(POWER_BUTTON_PIN) == LOW) {
    state |= (1 << BTN_POWER);
  }

  return state;
}

void InputManager::injectOneShotPress(const uint8_t buttonIndex) {
  if (buttonIndex <= BTN_POWER) {
    pendingInjectPress |= static_cast<uint8_t>(1u << buttonIndex);
  }
}

void InputManager::sample(const unsigned long currentTime, uint8_t& outPressed, uint8_t& outReleased) {
  const uint8_t state = getState();

  outPressed = 0;
  outReleased = 0;

  if (state != lastState) {
    lastDebounceTime = currentTime;
    lastState = state;
  }

  if ((currentTime - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (state != currentState) {
      outPressed = state & ~currentState;
      outReleased = currentState & ~state;

      if (outPressed > 0 && currentState == 0) {
        buttonPressStart = currentTime;
      }

      if (outReleased > 0 && state == 0) {
        buttonPressFinish = currentTime;
      }

      currentState = state;
    }
  }
}

void InputManager::poll() {
  uint8_t pressed = 0;
  uint8_t released = 0;
  sample(millis(), pressed, released);
  latchedPressed |= pressed;
  latchedReleased |= released;
}

void InputManager::update() {
  sample(millis(), pressedEvents, releasedEvents);

  if (latchedPressed != 0 || latchedReleased != 0) {
    pressedEvents |= latchedPressed;
    releasedEvents |= latchedReleased;
    latchedPressed = 0;
    latchedReleased = 0;
  }

  if (pendingInjectPress != 0) {
    pressedEvents |= pendingInjectPress;
    pendingInjectPress = 0;
  }
}

bool InputManager::isPressed(const uint8_t buttonIndex) const { return currentState & (1 << buttonIndex); }

bool InputManager::wasPressed(const uint8_t buttonIndex) const { return pressedEvents & (1 << buttonIndex); }

bool InputManager::wasAnyPressed() const { return pressedEvents > 0; }

bool InputManager::wasReleased(const uint8_t buttonIndex) const { return releasedEvents & (1 << buttonIndex); }

bool InputManager::wasAnyReleased() const { return releasedEvents > 0; }

unsigned long InputManager::getHeldTime() const {
  if (currentState > 0) {
    return millis() - buttonPressStart;
  }

  return buttonPressFinish - buttonPressStart;
}

const char* InputManager::getButtonName(const uint8_t buttonIndex) {
  if (buttonIndex <= BTN_POWER) {
    return BUTTON_NAMES[buttonIndex];
  }
  return "Unknown";
}

bool InputManager::isPowerButtonPressed() const { return isPressed(BTN_POWER); }
