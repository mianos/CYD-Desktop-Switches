#include "Display.h"

#include "Board.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace display {
namespace {

constexpr char TAG[] = "display";
constexpr ledc_channel_t kBlChannel = LEDC_CHANNEL_0;
constexpr ledc_channel_t kLedR = LEDC_CHANNEL_1;
constexpr ledc_channel_t kLedG = LEDC_CHANNEL_2;
constexpr ledc_channel_t kLedB = LEDC_CHANNEL_3;

esp_lcd_panel_handle_t s_panel = nullptr;
spi_device_handle_t s_touch = nullptr;
SemaphoreHandle_t s_colorDone = nullptr;

// Signalled from the SPI ISR when a queued colour transfer has finished.
bool onColorDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(ctx), &woken);
  return woken == pdTRUE;
}

// XPT2046 command bytes. X/Y use 12-bit differential mode; Z1/Z2 must be
// single-ended (SER/DFR=1) for the touch-pressure measurement to be valid.
constexpr uint8_t kCmdX = 0xD0;
constexpr uint8_t kCmdY = 0x90;
constexpr uint8_t kCmdZ1 = 0xB1;
constexpr uint8_t kCmdZ2 = 0xC1;

void initBacklight() {
  ledc_timer_config_t timer = {};
  timer.speed_mode = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_13_BIT;
  timer.timer_num = LEDC_TIMER_0;
  timer.freq_hz = 5000;
  timer.clk_cfg = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&timer));

  const gpio_num_t pins[4] = {board::kLcdBacklight, board::kLedR, board::kLedG, board::kLedB};
  for (int i = 0; i < 4; i++) {
    ledc_channel_config_t ch = {};
    ch.gpio_num = pins[i];
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel = static_cast<ledc_channel_t>(i);
    ch.timer_sel = LEDC_TIMER_0;
    ch.duty = (i == 0) ? 8191 : 0;  // backlight full-on, LEDs off
    ch.hpoint = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
  }
}

void initPanel() {
  spi_bus_config_t bus = {};
  bus.sclk_io_num = board::kLcdSclk;
  bus.mosi_io_num = board::kLcdMosi;
  bus.miso_io_num = board::kLcdMiso;  // shared with the touch controller
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = board::kWidth * board::kHeight * sizeof(uint16_t) + 8;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

  esp_lcd_panel_io_handle_t io = nullptr;
  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.dc_gpio_num = board::kLcdDc;
  io_cfg.cs_gpio_num = board::kLcdCs;
  io_cfg.pclk_hz = board::kLcdSpiHz;
  io_cfg.spi_mode = 0;
  io_cfg.trans_queue_depth = 10;
  io_cfg.on_color_trans_done = onColorDone;
  io_cfg.user_ctx = s_colorDone;
  io_cfg.lcd_cmd_bits = 8;
  io_cfg.lcd_param_bits = 8;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST), &io_cfg, &io));

  esp_lcd_panel_dev_config_t cfg = {};
  cfg.reset_gpio_num = GPIO_NUM_NC;  // no reset line; software reset
  cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  cfg.bits_per_pixel = 16;  // RGB565
  ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io, &cfg, &s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
  // Landscape: swap the axes, then apply the rotation that keeps this panel's
  // reversed column order straight (see board::kUpsideDown). Both calls only
  // rewrite MADCTL, so the order between them does not matter.
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, board::kMirrorX, board::kMirrorY));
}

void initTouch() {
  // The XPT2046 sits on its own SPI bus on the CYD: SCLK=25, MOSI=32, MISO=39.
  // (It is NOT on the display bus, which uses 13/12/14.)
  spi_bus_config_t bus = {};
  bus.sclk_io_num = board::kTouchSclk;
  bus.mosi_io_num = board::kTouchMosi;
  bus.miso_io_num = board::kTouchMiso;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 64;
  if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED) != ESP_OK) {
    s_touch = nullptr;
    return;
  }

  spi_device_interface_config_t dev = {};
  dev.mode = 0;
  dev.clock_speed_hz = board::kTouchSpiHz;
  dev.spics_io_num = board::kTouchCs;
  dev.queue_size = 1;
  if (spi_bus_add_device(SPI3_HOST, &dev, &s_touch) != ESP_OK) {
    s_touch = nullptr;
  }

  // PENIRQ (GPIO36) is an input-only pad with no internal pull-up; the CYD board
  // provides the external pull-up. Just make sure it is an input.
  gpio_config_t irq = {};
  irq.pin_bit_mask = 1ULL << board::kTouchIrq;
  irq.mode = GPIO_MODE_INPUT;
  irq.pull_up_en = GPIO_PULLUP_DISABLE;  // not available on this pad
  irq.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&irq);
}

uint16_t xptRead(uint8_t cmd) {
  if (!s_touch) return 0;
  uint8_t tx[3] = {cmd, 0x00, 0x00};
  uint8_t rx[3] = {0, 0, 0};
  spi_transaction_t t = {};
  t.length = 24;  // 8-bit command + 16 data bits
  t.tx_buffer = tx;
  t.rx_buffer = rx;
  // Polling avoids sharing the interrupt/queue path with the async LCD DMA.
  const esp_err_t err = spi_device_polling_transmit(s_touch, &t);
  if (err != ESP_OK) return 0;
  // 12-bit result: the converter's 12 data bits sit in the top of the 16 reply
  // bits, so shift right by 3 to land them in bits 11..0.
  return static_cast<uint16_t>((static_cast<uint16_t>(rx[1]) << 5) | (rx[2] >> 3));
}

}  // namespace

void init() {
  s_colorDone = xSemaphoreCreateBinary();
  initBacklight();
  initPanel();
  initTouch();
  ESP_LOGI(TAG, "ILI9341 %dx%d up, touch %s", board::kWidth, board::kHeight, s_touch ? "ok" : "missing");
}

void setBrightness(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, kBlChannel, 8191 * percent / 100);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, kBlChannel);
}

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  ledc_set_duty(LEDC_LOW_SPEED_MODE, kLedR, (uint32_t)r * 8191 / 255);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, kLedG, (uint32_t)g * 8191 / 255);
  ledc_set_duty(LEDC_LOW_SPEED_MODE, kLedB, (uint32_t)b * 8191 / 255);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, kLedR);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, kLedG);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, kLedB);
}

void flush(int y0, int y1, uint16_t* fb) {
  if (!s_panel) return;
  const int pixels = board::kWidth * (y1 - y0);
  // The ILI9341 driver ignores data_endian and sends the raw bytes, but the
  // panel wants big-endian RGB565 over SPI. Our buffer is little-endian, so
  // swap each pixel in place before handing it to the DMA.
  for (int i = 0; i < pixels; i++)
    fb[i] = static_cast<uint16_t>((fb[i] >> 8) | (fb[i] << 8));
  // draw_bitmap queues the transfer and returns immediately; wait for the ISR
  // so the caller can safely overwrite fb with the next band.
  xSemaphoreTake(s_colorDone, 0);
  esp_lcd_panel_draw_bitmap(s_panel, 0, y0, board::kWidth, y1, fb);
  xSemaphoreTake(s_colorDone, pdMS_TO_TICKS(200));
}

bool readTouch(uint16_t* x, uint16_t* y, uint32_t* pressure) {
  if (!s_touch) return false;
  const uint16_t z1 = xptRead(kCmdZ1);
  const uint16_t z2 = xptRead(kCmdZ2);
  if (x) *x = xptRead(kCmdX);
  if (y) *y = xptRead(kCmdY);
  if (pressure) *pressure = static_cast<uint32_t>(z1) + 4095 - z2;
  return true;
}

}  // namespace display
