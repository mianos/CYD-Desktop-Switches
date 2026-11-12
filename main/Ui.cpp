#include "Ui.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <regex>
#include <string>
#include <vector>

#include <cJSON.h>

#include "Board.h"
#include "Config.h"
#include "Display.h"
#include "JsonWrapper.h"
#include "MqttClient.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "font_gen.h"

namespace ui {
namespace {

constexpr char TAG[] = "ui";
// Touch calibration, measured corner-to-corner on this panel in the panel's
// native axes: raw X runs along the 240-px column axis but runs backwards (high
// at the left edge), raw Y runs down the 320-px row axis. Both were measured in
// portrait and are unchanged by the landscape framebuffer — touchToScreen()
// rotates the result — so recalibrating is still four numbers. The reversals
// live in board::kMirrorX/kMirrorY because the panel needs exactly the same
// ones.

// Raw 12-bit ADC values at native column x=0 / x=239 and row y=0 / y=319, taken
// from held corner presses.
constexpr int kRawXMin = 560, kRawXMax = 3530;
constexpr int kRawYMin = 320, kRawYMax = 3665;

// The panel is untouched when the Z figure is tiny (measured 2..192); a real
// press is in the thousands (measured 1483..3052).
constexpr uint32_t kPressThreshold = 400;

constexpr TickType_t kFrameMs = pdMS_TO_TICKS(20);

// Map a raw ADC reading in [inMin, inMax] onto [0, outMax], clamped.
int mapAxis(int v, int inMin, int inMax, int outMax) {
  int out = (v - inMin) * outMax / (inMax - inMin);
  if (out < 0) out = 0;
  if (out > outMax) out = outMax;
  return out;
}

// Convert raw touch coordinates into landscape screen pixels. The ADC keeps the
// panel's native axes, so map into native pixels first and rotate afterwards:
// with MV set, framebuffer X walks the native rows and framebuffer Y the native
// columns, and MX/MY (board::kMirrorX/kMirrorY) reverse those two. MX and MY are
// equal, so kUpsideDown flips both axes — the same 180° the panel gets — which
// is what keeps the picture and the hit boxes in step.
void touchToScreen(uint16_t rawX, uint16_t rawY, int* sx, int* sy) {
  const int col = mapAxis(rawX, kRawXMin, kRawXMax, board::kNativeW - 1);
  const int row = mapAxis(rawY, kRawYMin, kRawYMax, board::kNativeH - 1);
  *sx = board::kMirrorY ? (board::kNativeH - 1) - row : row;
  *sy = board::kMirrorX ? (board::kNativeW - 1) - col : col;
}

// The CYD (ESP32-WROOM-32) has no PSRAM, and Wi-Fi + MQTT leave too little RAM
// for a full 320x240 framebuffer, so the screen is drawn in horizontal bands.
// 24 rows x 320 px x 2 bytes is 15 KB of DMA-capable heap — less than the
// 40-row bands cost back when the screen was portrait.
constexpr int kBandH = 24;
constexpr int kBandPixels = board::kWidth * kBandH;

constexpr int kGap = 8;
constexpr int kStatusH = 38;

// Footer strip reserved at the bottom for the clock. It is taken out of the row
// stack, so a config with auto rows (height 0) gives the space back by itself:
// the default config's two auto rows split the whole content area between them.
constexpr int kFooterH = 26;

// Before 2020 the RTC is still at its 1970 default, i.e. SNTP has not set the
// clock yet — draw nothing rather than a wrong time.
constexpr time_t kClockValidAfter = 1600000000;

uint16_t* s_fb = nullptr;  // band buffer
int s_bandY0 = 0, s_bandY1 = 0;

MqttClient* s_mqtt = nullptr;
std::atomic<bool>* s_wifiUp = nullptr;
std::atomic<bool>* s_mqttUp = nullptr;

// Layout + wiring, straight out of the JSON config.
cfg::UiConfig s_cfg;

// Largest font face whose rendering of every label in a row fits (indexed like
// UiConfig::rowHeights), so all labels in a row are the same size.
std::vector<const font::Face*> s_rowFace;

// Set from the MQTT task when a state topic changed a button; the drawing loop
// picks it up and repaints.
std::atomic<bool> s_stateDirty{false};

// Momentary buttons have no state of their own, so a registered tap is shown by
// flashing the button briefly.
int s_flashIdx = -1;
TickType_t s_flashUntil = 0;
constexpr TickType_t kFlashMs = pdMS_TO_TICKS(350);

TickType_t s_lastQuery = 0;
bool s_queryNow = true;  // ask for every button's state once we are connected

constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Writes one pixel, clipped to the band currently being rendered.
inline void plot(int x, int y, uint16_t color) {
  if (x < 0 || x >= board::kWidth) return;
  if (y < s_bandY0 || y >= s_bandY1) return;
  s_fb[(y - s_bandY0) * board::kWidth + x] = color;
}

void fill(uint16_t color) {
  for (int i = 0; i < kBandPixels; i++) s_fb[i] = color;
}

void rect(int x, int y, int w, int h, uint16_t color) {
  if (x < 0) { w += x; x = 0; }
  if (x + w > board::kWidth) w = board::kWidth - x;
  for (int yy = y; yy < y + h; yy++)
    for (int xx = x; xx < x + w; xx++)
      plot(xx, yy, color);
}

void frame(int x, int y, int w, int h, uint16_t color) {
  rect(x, y, w, 1, color);
  rect(x, y + h - 1, w, 1, color);
  rect(x, y, 1, h, color);
  rect(x + w - 1, y, 1, h, color);
}

// Blends one glyph pixel of coverage 0-15 over whatever is already in the band.
void blend(int x, int y, uint16_t color, uint8_t alpha) {
  if (x < 0 || x >= board::kWidth || y < s_bandY0 || y >= s_bandY1) return;
  uint16_t& dst = s_fb[(y - s_bandY0) * board::kWidth + x];
  const int inv = 15 - alpha;
  const int dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
  const int sr = (color >> 11) & 0x1F, sg = (color >> 5) & 0x3F, sb = color & 0x1F;
  dst = static_cast<uint16_t>((((dr * inv + sr * alpha) / 15) << 11) |
                              (((dg * inv + sg * alpha) / 15) << 5) |
                              ((db * inv + sb * alpha) / 15));
}

// One glyph with its pen at (penX, baseline). Coverage values are 0-15.
void drawGlyph(const font::Face& face, int penX, int baseline, char c, uint16_t color) {
  const font::Glyph* g = font::glyph(face, c);
  if (!g || g->w == 0) return;
  const int sx = penX + g->left, sy = baseline + g->top;
  const int stride = (g->w + 1) / 2;
  for (int row = 0; row < g->h; row++) {
    const uint8_t* bits = face.bits + g->offset + row * stride;
    for (int col = 0; col < g->w; col++) {
      const uint8_t byte = bits[col >> 1];
      const uint8_t a = (col & 1) ? (byte & 0x0F) : (byte >> 4);
      if (a) blend(sx + col, sy + row, color, a);
    }
  }
}

int advanceOf(const font::Face& face, char c) {
  const font::Glyph* g = font::glyph(face, c);
  return g ? g->advance : face.ascent / 2;
}

int textW(const font::Face& face, const char* s) {
  int w = 0;
  for (; *s; s++) w += advanceOf(face, *s);
  return w;
}

void drawTextAt(const font::Face& face, int x, int baseline, const char* s,
                uint16_t color) {
  for (; *s; s++) {
    drawGlyph(face, x, baseline, *s, color);
    x += advanceOf(face, *s);
  }
}

// Same, but with `tracking` extra pixels between characters — used for the
// device name, where loose spacing reads as a wordmark instead of a word.
void drawTextTracked(const font::Face& face, int x, int baseline, const char* s,
                     uint16_t color, int tracking) {
  for (; *s; s++) {
    drawGlyph(face, x, baseline, *s, color);
    x += advanceOf(face, *s) + tracking;
  }
}

int textWTracked(const font::Face& face, const std::string& s, int tracking) {
  const int n = (int)s.size();
  return textW(face, s.c_str()) + (n > 1 ? tracking * (n - 1) : 0);
}

// A small filled disc: the connection LEDs in the status bar.
void disc(int cx, int cy, int r, uint16_t color) {
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++)
      if (dx * dx + dy * dy <= r * r + r) plot(cx + dx, cy + dy, color);
}

// Baseline that puts `face`'s line box in the middle of rows [y, y + h).
int centredBaseline(const font::Face& face, int y, int h) {
  return y + h / 2 + face.ascent - face.lineHeight / 2;
}

// Forgiving hit test: touches land a few pixels outside a box near the panel
// edges once the ADC is mapped to pixels.
constexpr int kHitSlop = 4;

bool inBox(int x, int y, int bx, int by, int bw, int bh) {
  return x >= bx - kHitSlop && x < bx + bw + kHitSlop &&
         y >= by - kHitSlop && y < by + bh + kHitSlop;
}

// Walk a dot separated path ("ZbReceived.0x1234.Power") through a JSON object.
cJSON* walkPath(cJSON* root, const std::string& path) {
  cJSON* cur = root;
  size_t start = 0;
  while (cur && start < path.size()) {
    size_t dot = path.find('.', start);
    if (dot == std::string::npos) dot = path.size();
    const std::string key = path.substr(start, dot - start);
    cur = key.empty() ? cur : cJSON_GetObjectItem(cur, key.c_str());
    start = dot + 1;
  }
  return cur;
}

// Escape regex metacharacters so a state topic matches literally. mqttwrapper
// dispatches on the first matching pattern and cannot unregister, so the UI
// registers exactly one handler per distinct state topic.
std::string regexEscape(const std::string& s) {
  static const char* special = R"(\.^$|()[]{}*+?)";
  std::string out;
  for (char c : s) {
    if (strchr(special, c)) out += '\\';
    out += c;
  }
  return out;
}

// A state topic carries JSON (mqttwrapper only dispatches JSON payloads). Every
// button watching this topic is updated by comparing the value at its `path`
// against the values it accepts as "on".
esp_err_t onState(MqttClient*, const std::string& topic, const JsonWrapper& doc, void*) {
  cJSON* root = cJSON_Parse(doc.ToString().c_str());
  if (!root) return ESP_OK;

  bool changed = false;
  for (auto& b : s_cfg.buttons) {
    if (!b.hasState() || b.stateTopic != topic) continue;
    const std::string value = cfg::scalarText(walkPath(root, b.statePath));
    if (value.empty()) continue;
    bool on = false;
    for (const auto& accepted : b.onValues) {
      if (accepted == value) { on = true; break; }
    }
    if (on != b.isOn) {
      b.isOn = on;
      changed = true;
      ESP_LOGI(TAG, "%s -> %s (from %s)", b.label.c_str(), on ? "ON" : "OFF", topic.c_str());
    }
  }

  cJSON_Delete(root);
  if (changed) s_stateDirty = true;
  return ESP_OK;
}

// A tap publishes exactly the payload the configuration gives for that button —
// a toggle sends `on` or `off` depending on what it believes the device is
// doing, so a state that changed behind the panel's back can never invert it.
void handleTap(int x, int y) {
  for (size_t i = 0; i < s_cfg.buttons.size(); i++) {
    auto& b = s_cfg.buttons[i];
    if (!inBox(x, y, b.x, b.y, b.w, b.h)) continue;

    const bool toggle = b.isToggle();
    const std::string payload = toggle ? (b.isOn ? b.off : b.on) : b.press;
    if (s_mqtt) s_mqtt->publish(b.topic, payload);
    ESP_LOGI(TAG, "tap '%s' -> %s %s", b.label.c_str(), b.topic.c_str(), payload.c_str());

    if (toggle) {
      b.isOn = !b.isOn;
    } else {
      s_flashIdx = (int)i;
      s_flashUntil = xTaskGetTickCount() + kFlashMs;
    }
    return;
  }
}

void drawScene() {
  const bool wUp = s_wifiUp && s_wifiUp->load();
  const bool mUp = s_mqttUp && s_mqttUp->load();
  const uint16_t ok = rgb565(0, 255, 0);
  const uint16_t white = rgb565(255, 255, 255);

  fill(rgb565(10, 11, 24));

  // Status bar: device name on the left with a small accent bar, connection
  // lights right-aligned. The coloured dot carries the state and the label
  // stays neutral, so the bar reads as chrome instead of shouting.
  rect(0, 0, board::kWidth, kStatusH, rgb565(17, 19, 32));
  rect(0, 0, board::kWidth, 1, rgb565(40, 44, 70));           // top highlight
  rect(0, kStatusH - 1, board::kWidth, 1, rgb565(5, 6, 12));  // bottom rule

  const uint16_t labelCol = rgb565(150, 158, 186);
  const uint16_t ledOk = rgb565(30, 220, 110), ledBad = rgb565(255, 80, 90);
  const int statusBase = centredBaseline(font::small, 0, kStatusH);

  // LED + label pairs, laid out from the right edge inwards.
  int x = board::kWidth - 10 - textW(font::small, "MQTT");
  drawTextAt(font::small, x, statusBase, "MQTT", labelCol);
  disc(x - 7, statusBase - 4, 2, mUp ? ledOk : ledBad);
  int statusLeft = x - 9;
  x = statusLeft - 12 - textW(font::small, "WIFI");
  drawTextAt(font::small, x, statusBase, "WIFI", labelCol);
  disc(x - 7, statusBase - 4, 2, wUp ? ledOk : ledBad);
  statusLeft = x - 9;

  // The name sits one step below the button labels: tracked, muted, and never
  // so wide that it runs under the LEDs.
  const int accentX = kGap + 4;
  rect(accentX, (kStatusH - 16) / 2, 3, 16, rgb565(0, 150, 255));

  constexpr int kTracking = 2;
  const int titleX = accentX + 9;
  const int room = statusLeft - 12 - titleX;
  std::string title = s_cfg.title;
  const font::Face* tf =
      textWTracked(font::mid, title, kTracking) <= room ? &font::mid : &font::small;
  int tw = 0;
  size_t keep = 0;
  for (; keep < title.size(); keep++) {
    const int add = advanceOf(*tf, title[keep]) + (keep ? kTracking : 0);
    if (tw + add > room) break;
    tw += add;
  }
  title.resize(keep);
  drawTextTracked(*tf, titleX, centredBaseline(*tf, 0, kStatusH), title.c_str(),
                  rgb565(198, 205, 228), kTracking);

  // One box per configured button: filled when a toggle is on, and flashed
  // briefly for a momentary press.
  for (size_t i = 0; i < s_cfg.buttons.size(); i++) {
    const auto& b = s_cfg.buttons[i];
    const bool flash = ((int)i == s_flashIdx);
    const bool lit = b.isToggle() && b.isOn;
    rect(b.x, b.y, b.w, b.h, lit ? rgb565(0, 130, 60) : rgb565(28, 30, 50));
    frame(b.x, b.y, b.w, b.h, (lit || flash) ? ok : rgb565(90, 92, 120));

    // Labels in a row share one face (chosen in layout()) so they line up.
    const font::Face& face = (b.row >= 0 && b.row < (int)s_rowFace.size())
                                 ? *s_rowFace[b.row]
                                 : font::big;
    const int baseline = centredBaseline(face, b.y, b.h);

    if (b.isToggle()) {
      const char* st = b.isOn ? "ON" : "OFF";
      const int stateW = textW(font::mid, st);
      drawTextAt(face, b.x + 10, baseline, b.label.c_str(), white);
      drawTextAt(font::mid, b.x + b.w - 10 - stateW, baseline, st,
                 b.isOn ? ok : rgb565(170, 170, 180));
    } else {
      drawTextAt(face, b.x + (b.w - textW(face, b.label.c_str())) / 2, baseline,
                 b.label.c_str(), white);
    }
  }

  // Footer: the clock, centred and quiet — it is a reference, not something to
  // read across the room, so it uses the same face as the ON/OFF state and no
  // seconds. Blank until SNTP has set the time (see startClock() in main.cpp).
  const time_t now = time(nullptr);
  if (now > kClockValidAfter) {
    struct tm stamp {};
    localtime_r(&now, &stamp);
    char clock[16];
    strftime(clock, sizeof clock, "%H:%M", &stamp);
    drawTextAt(font::mid, (board::kWidth - textW(font::mid, clock)) / 2,
               centredBaseline(font::mid, board::kHeight - kGap - kFooterH, kFooterH),
               clock, rgb565(150, 158, 186));
  }
}

// Pushes the whole scene to the panel one horizontal band at a time.
void render() {
  for (int band = 0; band < board::kHeight; band += kBandH) {
    s_bandY0 = band;
    s_bandY1 = (band + kBandH < board::kHeight) ? band + kBandH : board::kHeight;
    drawScene();
    display::flush(s_bandY0, s_bandY1, s_fb);
  }
}

// Smallest row the automatic share will hand out before every row is scaled
// down instead.
constexpr int kMinRowH = 16;

// Fit the configured row heights into `usable` px. Auto rows (height 0) share
// whatever the fixed rows leave over; if the fixed rows alone overflow, every
// row is scaled down instead. Without this a config authored for a taller
// screen — an NVS copy from before the panel was rotated, say — would stack its
// rows under the footer, where they can be neither read nor tapped.
void fitRows(std::vector<int>& heights, int usable) {
  int fixed = 0, autos = 0;
  for (int h : heights) {
    if (h > 0) fixed += h;
    else autos++;
  }
  const int spare = usable - fixed;
  if (autos > 0 && spare / autos >= kMinRowH) {
    for (int& h : heights)
      if (h <= 0) h = spare / autos;
    return;
  }
  if (autos == 0 && spare >= 0) return;  // fixed rows already fit
  // Not enough room, even for the minimum: scale every row by the same factor.
  // Rows end up small, but a row drawn below the footer can never be tapped at
  // all, so fitting badly beats not fitting.
  const int want = fixed + autos * kMinRowH;
  ESP_LOGW(TAG, "row heights scaled to fit %d px (config asks for %d)", usable, want);
  for (int& h : heights) {
    const int base = h > 0 ? h : kMinRowH;
    h = base * usable / (want > 0 ? want : 1);
    if (h < 1) h = 1;
  }
}

// Stacks the configured rows and splits each one into equal-width buttons.
void layout() {
  const int rows = (int)s_cfg.rowHeights.size();
  s_rowFace.assign(s_cfg.rowHeights.size(), &font::big);
  const int top = kStatusH + kGap;
  const int bottom = board::kHeight - kGap - kFooterH;
  const int usable = bottom - top - (rows > 0 ? (rows - 1) * kGap : 0);
  std::vector<int> heights = s_cfg.rowHeights;
  fitRows(heights, usable);

  int y = top;
  for (int r = 0; r < rows; r++) {
    const int h = heights[r];
    std::vector<size_t> inRow;
    for (size_t i = 0; i < s_cfg.buttons.size(); i++)
      if (s_cfg.buttons[i].row == r) inRow.push_back(i);

    const int n = (int)inRow.size();
    if (n > 0) {
      const int w = (board::kWidth - 2 * kGap - (n - 1) * kGap) / n;
      for (int k = 0; k < n; k++) {
        auto& b = s_cfg.buttons[inRow[k]];
        b.x = kGap + k * (w + kGap);
        b.y = y;
        b.w = w;
        b.h = h;
      }

      // Labels in a row share one face: the largest that every label in the
      // row fits, so a long label shrinks its neighbours instead of looking
      // mismatched.
      int avail = w;
      for (size_t idx : inRow) {
        const auto& b = s_cfg.buttons[idx];
        const int room = b.isToggle() ? b.w - 20 - textW(font::mid, "OFF") : b.w - 12;
        if (room < avail) avail = room;
      }
      const font::Face* face = &font::big;
      for (size_t idx : inRow)
        if (textW(font::big, s_cfg.buttons[idx].label.c_str()) > avail) face = &font::mid;
      for (size_t idx : inRow)
        if (textW(*face, s_cfg.buttons[idx].label.c_str()) > avail) face = &font::small;
      s_rowFace[r] = face;
    }
    y += h + kGap;
  }
}

// Subscribe to every distinct state topic the config asks for.
void registerStateHandlers() {
  if (!s_mqtt) return;
  std::vector<std::string> done;
  for (const auto& b : s_cfg.buttons) {
    if (!b.hasState()) continue;
    bool already = false;
    for (const auto& topic : done)
      if (topic == b.stateTopic) { already = true; break; }
    if (already) continue;
    done.push_back(b.stateTopic);
    s_mqtt->registerHandler(b.stateTopic, std::regex(regexEscape(b.stateTopic)), onState, nullptr);
  }
  ESP_LOGI(TAG, "watching %u state topic(s)", (unsigned)done.size());
}

}  // namespace

void init(const cfg::UiConfig& config, MqttClient* mqtt,
          std::atomic<bool>* wifiUp, std::atomic<bool>* mqttUp) {
  s_cfg = config;
  s_mqtt = mqtt;
  s_wifiUp = wifiUp;
  s_mqttUp = mqttUp;
  if (s_cfg.title.empty()) s_cfg.title = "panel";

  layout();
  for (const auto& b : s_cfg.buttons)
    ESP_LOGI(TAG, "button '%s' %s at %d,%d %dx%d -> %s", b.label.c_str(),
             b.isToggle() ? "toggle" : "momentary", b.x, b.y, b.w, b.h, b.topic.c_str());
  registerStateHandlers();
}

void run() {
  s_fb = static_cast<uint16_t*>(heap_caps_malloc(kBandPixels * sizeof(uint16_t), MALLOC_CAP_DMA));
  if (!s_fb) {
    ESP_LOGE(TAG, "band buffer alloc failed (%d bytes)", kBandPixels * (int)sizeof(uint16_t));
    return;
  }

  bool dirty = true;  // render once at startup
  bool wasPressed = false;
  bool wasWifi = false, wasMqtt = false;
  long lastMinute = -1;  // footer clock, -1 = not synced yet
  const TickType_t pollTicks = pdMS_TO_TICKS((uint32_t)s_cfg.pollSeconds * 1000);

  while (true) {
    uint16_t rawX = 0, rawY = 0;
    uint32_t pressure = 0;
    display::readTouch(&rawX, &rawY, &pressure);
    const int irqLevel = gpio_get_level(board::kTouchIrq);

    int px = 0, py = 0;
    touchToScreen(rawX, rawY, &px, &py);

    // A press needs the pressure figure as well as PENIRQ, so a floating or
    // noisy IRQ edge cannot fire a phantom tap.
    const bool pressed = irqLevel == 0 && pressure > kPressThreshold;
    const int x = px, y = py;

    if (pressed && !wasPressed) {
      ESP_LOGI(TAG, "tap raw=(%u,%u) -> screen=(%d,%d) p=%u", rawX, rawY, x, y, (unsigned)pressure);
      handleTap(x, y);
      dirty = true;
    }
    wasPressed = pressed;

    // Clear a momentary button's highlight once it has been shown long enough.
    if (s_flashIdx >= 0 && (int32_t)(xTaskGetTickCount() - s_flashUntil) >= 0) {
      s_flashIdx = -1;
      dirty = true;
    }

    // Only touch the LED + screen when the network state changes.
    const bool wUp = s_wifiUp && s_wifiUp->load();
    const bool mUp = s_mqttUp && s_mqttUp->load();
    const bool mqttJustUp = mUp && !wasMqtt;
    if (wUp != wasWifi || mUp != wasMqtt) {
      wasWifi = wUp;
      wasMqtt = mUp;
      if (wUp && mUp) display::setLed(0, 255, 0);
      else if (wUp) display::setLed(255, 165, 0);
      else display::setLed(255, 0, 0);
      dirty = true;
    }

    // Re-send every configured query just after connecting and then every
    // poll_s seconds, so a state change missed while the link was down repairs
    // itself without any per-device code.
    if (mqttJustUp) s_queryNow = true;
    const bool pollDue = pollTicks > 0 && (xTaskGetTickCount() - s_lastQuery) >= pollTicks;
    if (s_queryNow || pollDue) {
      if (s_mqtt && mUp) {
        s_queryNow = false;
        s_lastQuery = xTaskGetTickCount();
        for (const auto& b : s_cfg.buttons)
          if (!b.queryTopic.empty()) s_mqtt->publish(b.queryTopic, b.queryPayload);
      }
    }

    // The footer clock only changes on the minute, so repaint on the tick
    // rather than holding a timer — and once more when SNTP first lands, which
    // is the -1 -> real-minute transition.
    const time_t now = time(nullptr);
    const long minute = now > kClockValidAfter ? static_cast<long>(now / 60) : -1;
    if (minute != lastMinute) {
      lastMinute = minute;
      dirty = true;
    }

    if (s_stateDirty.exchange(false)) dirty = true;

    // Render only when something actually changed. No periodic redraw — the
    // screen stays still otherwise.
    if (dirty) {
      render();
      dirty = false;
    }

    vTaskDelay(kFrameMs);
  }
}

}  // namespace ui
