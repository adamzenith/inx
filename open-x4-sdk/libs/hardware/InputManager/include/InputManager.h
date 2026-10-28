#pragma once

/**
 * @file InputManager.h
 * @brief Public interface and types for InputManager.
 */

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class InputManager {
 public:
  InputManager();
  void begin();
  uint8_t getState();

  /**
   * Updates the button states. Should be called regularly in the main loop.
   */
  void update();

  /** Queue a one-shot press for the next update() (e.g. BLE HID → same path as physical buttons). */
  void injectOneShotPress(uint8_t buttonIndex);

  /**
   * Returns true if the button was being held at the time of the last #update() call.
   *
   * @param buttonIndex the button indexes
   * @return the button current press state
   */
  bool isPressed(uint8_t buttonIndex) const;

  /**
   * Returns true if the button went from unpressed to pressed between the last two #update() calls.
   *
   * This differs from #isPressed() in that pressing and holding a button will cause this function
   * to return true after the first #update() call, but false on subsequent calls, whereas #isPressed()
   * will continue to return true.
   *
   * @param buttonIndex
   * @return the button pressed state
   */
  bool wasPressed(uint8_t buttonIndex) const;

  /**
   * Returns true if any button started being pressed between the last two #update() calls
   *
   * @return true if any button started being pressed between the last two #update() calls
   */
  bool wasAnyPressed() const;

  /**
   * Returns true if the button went from pressed to unpressed between the last two #update() calls
   *
   * @param buttonIndex the button indexes
   * @return the button release state
   */
  bool wasReleased(uint8_t buttonIndex) const;

  /**
   * Returns true if any button was released between the last two #update() calls
   *
   * @return  true if any button was released between the last two #update() calls
   */
  bool wasAnyReleased() const;

  /**
   * Returns the time between any button starting to be depressed and all buttons between released
   *
   * @return duration in milliseconds
   */
  unsigned long getHeldTime() const;

  static constexpr uint8_t BTN_BACK = 0;
  static constexpr uint8_t BTN_CONFIRM = 1;
  static constexpr uint8_t BTN_LEFT = 2;
  static constexpr uint8_t BTN_RIGHT = 3;
  static constexpr uint8_t BTN_UP = 4;
  static constexpr uint8_t BTN_DOWN = 5;
  static constexpr uint8_t BTN_POWER = 6;

  static constexpr int BUTTON_ADC_PIN_1 = 1;
  static constexpr int BUTTON_ADC_PIN_2 = 2;
  static constexpr int POWER_BUTTON_PIN = 3;

  bool isPowerButtonPressed() const;

  /**
   * Samples the buttons without consuming events: any press/release seen here is latched and reported by
   * the next #update(). Lets a long blocking operation (e.g. an e-ink refresh) keep watching the buttons so
   * a tap that starts and ends inside it isn't lost.
   *
   * Safe to call from any task: sampling is serialised with #update(), and a poll that finds an #update()
   * in progress is skipped, since that update is sampling anyway.
   */
  void poll();

  /** True if a button is down, or a press/release latched by #poll() is waiting for the next #update(). */
  bool hasPendingActivity() const { return currentState != 0 || latchedPressed != 0 || latchedReleased != 0; }

  static const char* getButtonName(uint8_t buttonIndex);

 private:
  int getButtonFromADC(int adcValue, const int ranges[], int numButtons);
  void sample(unsigned long currentTime, uint8_t& outPressed, uint8_t& outReleased);

  uint8_t currentState;
  uint8_t lastState;
  uint8_t pressedEvents;
  uint8_t releasedEvents;
  uint8_t pendingInjectPress{0};
  uint8_t latchedPressed{0};
  uint8_t latchedReleased{0};
  unsigned long lastDebounceTime;
  unsigned long buttonPressStart;
  unsigned long buttonPressFinish;
  // Serialises sample() and the latch hand-over between #poll() on another task and #update() on the main
  // loop. A mutex rather than a critical section: analogRead() may block on the ADC driver's own lock.
  SemaphoreHandle_t sampleMutex = nullptr;

  static constexpr int NUM_BUTTONS_1 = 4;
  static const int ADC_RANGES_1[];

  static constexpr int NUM_BUTTONS_2 = 2;
  static const int ADC_RANGES_2[];

  static constexpr int ADC_NO_BUTTON = 3800;
  static constexpr unsigned long DEBOUNCE_DELAY = 5;

  static const char* BUTTON_NAMES[];
};
