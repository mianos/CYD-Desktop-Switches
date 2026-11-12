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

// Orientation. The glass and the touch ADC keep the panel's native portrait
// axes; the framebuffer is landscape. MADCTL after the init sequence is 0x08
// (BGR only), and this panel is wired with its column order reversed, so the
// picture only comes out the right way round when an odd number of MX/MY/MV is
// set — exactly the four rotations TFT_eSPI lists for an ILI9341: 0x48 (MX),
// 0x28 (MV), 0x88 (MY), 0xE8 (MV|MX|MY). Portrait used 0x48. Landscape needs MV
// (it is what puts the 320-px native row axis on the framebuffer's X), and under
// MV the other two odd-parity entries have MX and MY *equal*, not complementary:
// MX alone is 0x68, which is not a rotation but the mirror image of 0xE8.
//   true:  MV|MX|MY -> 0xE8 — what this build ships: native left edge of the
//                      glass at the top, text upright and reading left to right
//   false: MV only   -> 0x28, the same picture turned 180°
constexpr bool kUpsideDown = true;
constexpr bool kMirrorX = kUpsideDown;  // MX
constexpr bool kMirrorY = kUpsideDown;  // MY

constexpr int kNativeW = 240;  // the glass, as the panel addresses it
constexpr int kNativeH = 320;
constexpr int kWidth  = kNativeH;  // the framebuffer, as this code draws it
constexpr int kHeight = kNativeW;

}  // namespace board
