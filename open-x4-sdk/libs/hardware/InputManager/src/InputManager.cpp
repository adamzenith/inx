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
    taskENTER_CRITICAL(&latchMux);
    pendingInjectPress |= static_cast<uint8_t>(1u << buttonIndex);
    taskEXIT_CRITICAL(&latchMux);
  }
}

void InputManager::sampleOnce() {
  const unsigned long currentTime = millis();
  // getState() (analogRead) must stay outside the critical section.
  const uint8_t state = getState();

  if (state != lastState) {
    lastDebounceTime = currentTime;
    lastState = state;
  }

  if ((currentTime - lastDebounceTime) <= DEBOUNCE_DELAY || state == currentState) {
    return;
  }

  const uint8_t pressed = state & ~currentState;
  const uint8_t released = currentState & ~state;

  if (pressed > 0 && currentState == 0) {
    buttonPressStart = currentTime;
  }

  if (released > 0 && state == 0) {
    buttonPressFinish = currentTime;
  }

  currentState = state;

  taskENTER_CRITICAL(&latchMux);
  pendingPressed |= pressed;
  pendingReleased |= released;
  for (uint8_t i = 0; i < NUM_BUTTONS_TOTAL; i++) {
    if ((pressed & (1u << i)) != 0 && pendingPressCounts[i] < MAX_PRESS_COUNT) {
      pendingPressCounts[i]++;
    }
  }
  taskEXIT_CRITICAL(&latchMux);
}

void InputManager::update() {
  if (samplingTask == nullptr) {
    sampleOnce();
  }

  taskENTER_CRITICAL(&latchMux);
  pressedEvents = pendingPressed | pendingInjectPress;
  releasedEvents = pendingReleased;
  pendingPressed = 0;
  pendingReleased = 0;
  pendingInjectPress = 0;
  for (uint8_t i = 0; i < NUM_BUTTONS_TOTAL; i++) {
    consumedPressCounts[i] = pendingPressCounts[i];
    pendingPressCounts[i] = 0;
  }
  heldStart = buttonPressStart;
  heldFinish = buttonPressFinish;
  taskEXIT_CRITICAL(&latchMux);
}

uint8_t InputManager::takePressCount(const uint8_t buttonIndex) {
  if (buttonIndex >= NUM_BUTTONS_TOTAL) {
    return 0;
  }
  const uint8_t count = consumedPressCounts[buttonIndex];
  consumedPressCounts[buttonIndex] = 0;
  return count;
}

void InputManager::samplingTaskTrampoline(void* arg) {
  auto* self = static_cast<InputManager*>(arg);
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    self->sampleOnce();
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
  }
}

bool InputManager::startSamplingTask() {
  if (samplingTask != nullptr) {
    return true;
  }
  return xTaskCreate(&InputManager::samplingTaskTrampoline, "btnSample", SAMPLING_TASK_STACK, this,
                     SAMPLING_TASK_PRIO, &samplingTask) == pdPASS;
}

void InputManager::stopSamplingTask() {
  if (samplingTask == nullptr) {
    return;
  }
  vTaskDelete(samplingTask);
  samplingTask = nullptr;
}

bool InputManager::isPressed(const uint8_t buttonIndex) const { return currentState & (1 << buttonIndex); }

bool InputManager::wasPressed(const uint8_t buttonIndex) const { return pressedEvents & (1 << buttonIndex); }

bool InputManager::wasAnyPressed() const { return pressedEvents > 0; }

bool InputManager::wasReleased(const uint8_t buttonIndex) const { return releasedEvents & (1 << buttonIndex); }

bool InputManager::wasAnyReleased() const { return releasedEvents > 0; }

unsigned long InputManager::getHeldTime() const {
  if (currentState > 0) {
    return millis() - heldStart;
  }

  return heldFinish - heldStart;
}

const char* InputManager::getButtonName(const uint8_t buttonIndex) {
  if (buttonIndex <= BTN_POWER) {
    return BUTTON_NAMES[buttonIndex];
  }
  return "Unknown";
}

bool InputManager::isPowerButtonPressed() const { return isPressed(BTN_POWER); }
