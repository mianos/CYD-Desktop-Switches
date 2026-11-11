#pragma once
#include <cstdint>

// ILI9341 panel, TFT backlight, RGB LED and XPT2046 touch. The panel has no
// dedicated reset line (it's tied to EN), so the driver uses a software reset.
namespace display {

// Brings up the backlight, panel and RGB LED. Touch is best-effort: a missing
// or unresponsive controller is logged but never stops the panel.
void init();

void setBrightness(int percent);               // 0-100
void setLed(uint8_t r, uint8_t g, uint8_t b);  // 0-255 per channel

// Pushes one horizontal band [y0, y1) of an RGB565 framebuffer to the panel.
// The buffer holds board::kWidth * (y1 - y0) pixels. It is byte-swapped in
// place (the panel expects big-endian RGB565 over SPI) and returns only once
// the transfer has finished, so it is safe to reuse the buffer afterwards.
void flush(int y0, int y1, uint16_t* fb);

// Reads one raw XPT2046 sample (x, y and touch pressure). Returns false when the
// touch controller is absent or the SPI transaction failed.
bool readTouch(uint16_t* x, uint16_t* y, uint32_t* pressure);

}  // namespace display
