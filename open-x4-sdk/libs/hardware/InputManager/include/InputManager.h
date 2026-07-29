#pragma once

/**
 * @file InputManager.h
 * @brief Public interface and types for InputManager.
 */

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class InputManager {
 public:
  InputManager();
  void begin();
  uint8_t getState();

  /**
   * Consumes the edges latched by the background sampler into the wasPressed()/wasReleased()
   * masks. Should be called regularly in the main loop. Falls back to sampling inline when the
   * sampling task is not running (early boot, or task creation failed).
   */
  void update();

  /**
   * Samples the buttons once and latches debounced press/release edges. Called periodically by
   * the sampling task; update() calls it inline when no task is running.
   */
  void sampleOnce();

  /**
   * Starts the background sampling task (~10 ms period). Idempotent. Returns false if the task
   * could not be created, in which case update() keeps sampling inline.
   */
  bool startSamplingTask();

  /** Stops the sampling task (deep-sleep entry). Safe to call when not started. */
  void stopSamplingTask();

  /**
   * Debounced presses of this button latched since the previous #update(), consumed by this
   * call. Same lifetime as #wasPressed(): whatever is not drained is discarded by the next
   * #update().
   */
  uint8_t takePressCount(uint8_t buttonIndex);

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
   * Returns true if the button went from unpressed to pressed since the last #update() call.
   * Edges are latched by the background sampler, so presses during blocking work are not lost.
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

  static const char* getButtonName(uint8_t buttonIndex);

 private:
  static constexpr uint8_t NUM_BUTTONS_TOTAL = 7;
  static constexpr uint8_t MAX_PRESS_COUNT = 15;
  static constexpr uint32_t SAMPLE_PERIOD_MS = 10;
  static constexpr uint32_t SAMPLING_TASK_STACK = 2560;
  static constexpr UBaseType_t SAMPLING_TASK_PRIO = 3;

  int getButtonFromADC(int adcValue, const int ranges[], int numButtons);
  static void samplingTaskTrampoline(void* arg);

  // Sampler-owned (written only by sampleOnce; single-byte/word reads elsewhere are atomic on RV32)
  uint8_t currentState;
  uint8_t lastState;
  unsigned long lastDebounceTime;
  unsigned long buttonPressStart;
  unsigned long buttonPressFinish;

  // Latches shared between the sampler and update(); guarded by latchMux
  volatile uint8_t pendingPressed{0};
  volatile uint8_t pendingReleased{0};
  volatile uint8_t pendingPressCounts[NUM_BUTTONS_TOTAL]{};
  uint8_t pendingInjectPress{0};

  // Main-task-only consumed state
  uint8_t pressedEvents;
  uint8_t releasedEvents;
  uint8_t consumedPressCounts[NUM_BUTTONS_TOTAL]{};
  unsigned long heldStart{0};
  unsigned long heldFinish{0};

  TaskHandle_t samplingTask{nullptr};
  portMUX_TYPE latchMux = portMUX_INITIALIZER_UNLOCKED;

  static constexpr int NUM_BUTTONS_1 = 4;
  static const int ADC_RANGES_1[];

  static constexpr int NUM_BUTTONS_2 = 2;
  static const int ADC_RANGES_2[];

  static constexpr int ADC_NO_BUTTON = 3800;
  static constexpr unsigned long DEBOUNCE_DELAY = 5;

  static const char* BUTTON_NAMES[];
};
