#pragma once
#include "driver/gpio.h"

// Cheap Yellow Display (ESP32-2432S028R): ESP32-WROOM-32 + 2.8" ILI9341 touch.
// Pins are fixed by the PCB, so there is nothing to configure.
namespace board {

constexpr gpio_num_t kLcdMosi = GPIO_NUM_13;
constexpr gpio_num_t kLcdMiso = GPIO_NUM_12;
constexpr gpio_num_t kLcdSclk = GPIO_NUM_14;
constexpr gpio_num_t kLcdCs   = GPIO_NUM_15;
constexpr gpio_num_t kLcdDc   = GPIO_NUM_2;
constexpr gpio_num_t kLcdBacklight = GPIO_NUM_21;

constexpr gpio_num_t kTouchCs = GPIO_NUM_33;  // XPT2046, on its own SPI bus
// The XPT2046 is NOT on the display SPI bus on the CYD — it has dedicated pins.
constexpr gpio_num_t kTouchSclk = GPIO_NUM_25;
constexpr gpio_num_t kTouchMosi = GPIO_NUM_32;
constexpr gpio_num_t kTouchMiso = GPIO_NUM_39;  // input-only pin, fine for MISO
constexpr gpio_num_t kTouchIrq = GPIO_NUM_36;

constexpr gpio_num_t kLedR = GPIO_NUM_4;
constexpr gpio_num_t kLedG = GPIO_NUM_16;
constexpr gpio_num_t kLedB = GPIO_NUM_17;

constexpr int kLcdSpiHz = 40 * 1000 * 1000;
constexpr int kTouchSpiHz = 2 * 1000 * 1000;  // XPT2046 tops out ~2.5 MHz

constexpr int kWidth = 240;
constexpr int kHeight = 320;

}  // namespace board
