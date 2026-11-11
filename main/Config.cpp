#include "Config.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_log.h"

// Embedded by EMBED_TXTFILES in main/CMakeLists.txt.
extern const char def_cfg_start[] asm("_binary_default_config_json_start");
extern const char def_cfg_end[] asm("_binary_default_config_json_end");

namespace cfg {
namespace {

constexpr char TAG[] = "cfg";

std::string textOf(const cJSON* item) {
    if (!item) return {};
    return scalarText(item);
}

// Pull the object field `key` out of `parent` as a plain string.
std::string str(const cJSON* parent, const char* key) {
    if (!parent) return {};
    return textOf(cJSON_GetObjectItem(parent, key));
}

// `on` may be a single scalar or an array of them.
void collectOnValues(const cJSON* item, std::vector<std::string>& out) {
    if (!item) return;
    if (cJSON_IsArray(item)) {
        const cJSON* v = nullptr;
        cJSON_ArrayForEach(v, item) out.push_back(scalarText(v));
    } else {
        out.push_back(scalarText(item));
    }
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const std::string& s) { return s.empty(); }),
              out.end());
}

bool parseButton(const cJSON* item, int row, UiConfig& out, std::string& err) {
    Button b;
    b.row = row;
    b.label = str(item, "label");

    const cJSON* publish = cJSON_GetObjectItem(item, "publish");
    b.topic = str(publish, "topic");
    b.on    = str(publish, "on");
    b.off   = str(publish, "off");
    b.press = str(publish, "press");

    const cJSON* state = cJSON_GetObjectItem(item, "state");
    if (cJSON_IsObject(state)) {
        b.stateTopic = str(state, "topic");
        b.statePath  = str(state, "path");
        collectOnValues(cJSON_GetObjectItem(state, "on"), b.onValues);
        if (!b.stateTopic.empty() && b.onValues.empty()) b.onValues.emplace_back("ON");
    }

    const cJSON* query = cJSON_GetObjectItem(item, "query");
    if (cJSON_IsObject(query)) {
        b.queryTopic   = str(query, "topic");
        b.queryPayload = str(query, "payload");
    }

    if (b.label.empty()) {
        err = "button without a label";
        return false;
    }
    if (b.topic.empty()) {
        err = "'" + b.label + "' has no publish.topic";
        return false;
    }
    if (b.press.empty() && (b.on.empty() || b.off.empty())) {
        err = "'" + b.label + "' needs publish.on + publish.off (toggle) or publish.press (momentary)";
        return false;
    }
    out.buttons.push_back(std::move(b));
    return true;
}

}  // namespace

std::string scalarText(const cJSON* item) {
    if (!item) return {};
    if (cJSON_IsString(item)) return item->valuestring ? item->valuestring : "";
    if (cJSON_IsBool(item)) return cJSON_IsTrue(item) ? "true" : "false";
    if (cJSON_IsNumber(item)) {
        char buf[32];
        // %g keeps integers integral (1 rather than 1.000000).
        snprintf(buf, sizeof buf, "%g", item->valuedouble);
        return buf;
    }
    return {};
}

bool parse(const std::string& json, UiConfig& out, std::string& err) {
    out = UiConfig{};
    err.clear();

    if (json.empty()) {
        err = "empty config";
        return false;
    }

    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) {
        const char* at = cJSON_GetErrorPtr();
        err = std::string("invalid JSON near: ") + (at ? at : "?");
        return false;
    }

    const cJSON* rows = cJSON_GetObjectItem(root, "rows");
    if (!cJSON_IsArray(rows)) {
        err = "missing 'rows' array";
        cJSON_Delete(root);
        return false;
    }

    bool ok = true;
    int rowIdx = 0;
    const cJSON* row = nullptr;
    cJSON_ArrayForEach(row, rows) {
        if (rowIdx >= (int)kMaxRows) {
            err = "too many rows";
            ok = false;
            break;
        }
        const cJSON* height = cJSON_GetObjectItem(row, "height");
        out.rowHeights.push_back(cJSON_IsNumber(height) ? (int)height->valuedouble : 0);

        const cJSON* buttons = cJSON_GetObjectItem(row, "buttons");
        if (!cJSON_IsArray(buttons)) {
            err = "row " + std::to_string(rowIdx) + " has no 'buttons' array";
            ok = false;
            break;
        }
        const cJSON* button = nullptr;
        cJSON_ArrayForEach(button, buttons) {
            if (out.buttons.size() >= kMaxButtons) {
                err = "too many buttons";
                ok = false;
                break;
            }
            if (!parseButton(button, rowIdx, out, err)) {
                ok = false;
                break;
            }
        }
        if (!ok) break;
        rowIdx++;
    }

    if (ok) {
        const cJSON* title = cJSON_GetObjectItem(root, "title");
        if (cJSON_IsString(title)) out.title = title->valuestring;

        const cJSON* poll = cJSON_GetObjectItem(root, "poll_s");
        if (cJSON_IsNumber(poll)) {
            const int secs = (int)poll->valuedouble;
            out.pollSeconds = secs > 0 ? secs : 0;  // 0 disables periodic queries
        }
        if (out.buttons.empty()) {
            err = "config has no buttons";
            ok = false;
        }
    }

    cJSON_Delete(root);
    if (!ok) out = UiConfig{};
    return ok;
}

const char* defaultJson() {
    static std::string text;
    if (text.empty()) {
        // EMBED_TXTFILES appends a NUL; drop it and any newline padding.
        size_t len = (size_t)(def_cfg_end - def_cfg_start);
        while (len > 0 && (def_cfg_start[len - 1] == '\0' || def_cfg_start[len - 1] == '\n')) len--;
        text.assign(def_cfg_start, len);
    }
    return text.c_str();
}

}  // namespace cfg
