#pragma once

#include <string>
#include <vector>

#include <cJSON.h>

// What the panel is and does, described entirely by JSON (default_config.json).
//
// The firmware knows nothing about Zigbee, Tasmota or any particular light: a
// button is a label, a topic to publish to, and — optionally — a state topic
// that decides how the button is drawn.
//
//   {
//     "title": "desk",              // status-bar caption (default: sensor_name)
//     "poll_s": 60,                 // re-send every button's query this often
//     "rows": [
//       { "height": 64,             // pixels; omit or 0 to share leftover height
//         "buttons": [
//           { "label": "LIGHT",
//             "publish": { "topic": "cmnd/light/POWER",
//                          "on": "ON", "off": "OFF" },     // toggle button
//             "state":   { "topic": "stat/light/RESULT",
//                          "path": "POWER", "on": ["ON"] },
//             "query":   { "topic": "cmnd/light/Power" } } ] },
//       { "height": 88,
//         "buttons": [
//           { "label": "UP",          // "press" instead of on/off: momentary
//             "publish": { "topic": "cmnd/zigbridge/ZbSend",
//                          "press": "{\"Device\":\"0x5678\",\"Write\":{\"EF00/0401\":0}}" } } ] }
//     ]
//   }
namespace cfg {

// Maximum sizes — a malformed or hostile config must not exhaust RAM.
constexpr size_t kMaxRows = 8;
constexpr size_t kMaxButtons = 16;

struct Button {
    std::string label;

    // What a tap publishes. on/off make a toggle, press makes it momentary.
    std::string topic;
    std::string on, off, press;

    // Optional state feedback: walk `statePath` (dot separated) into the JSON
    // received on `stateTopic` and compare it against `onValues`.
    std::string stateTopic, statePath;
    std::vector<std::string> onValues;  // normalised scalars, e.g. {"ON", "1", "true"}

    // Optional query, published on connect and every poll_s seconds.
    std::string queryTopic, queryPayload;

    // Layout, filled in by ui::init(). `row` indexes UiConfig::rowHeights.
    int row = 0, x = 0, y = 0, w = 0, h = 0;
    bool isOn = false;  // last known state (from `state`, or the last tap)

    bool isToggle() const { return press.empty(); }
    bool hasState() const { return !stateTopic.empty(); }
};

struct UiConfig {
    std::string title;
    int pollSeconds = 60;
    std::vector<int> rowHeights;  // pixels, 0 = auto
    std::vector<Button> buttons;
};

// Parses and validates `json`. Returns false and fills `err` on bad JSON or a
// schema problem, leaving `out` empty so the caller can fall back.
bool parse(const std::string& json, UiConfig& out, std::string& err);

// JSON text of a scalar (string as-is, number/bool as their JSON spelling);
// empty for objects, arrays, null and missing values.
std::string scalarText(const cJSON* item);

// The compiled-in default: main/default_config.json, embedded at build time.
const char* defaultJson();

}  // namespace cfg
