#pragma once

#include <string>

#include "SettingsBase.h"

// Desk-controller settings schema — persistence lives in mianesp's settingsbase.
// Member initialisers are the compiled-in defaults; missing or unparseable NVS
// config falls back to them (logged).
//
// Only the things the panel cannot work without live here: where the broker is
// and what to call itself. Everything the panel *shows and does* is described
// by the JSON config (see Config.h), which is stored separately in NVS so it
// can be replaced over MQTT without touching these fields.
struct Settings : SettingsBase {
    std::string mqttServer = "";
    int         mqttPort   = 1883;
    std::string mqttUser   = "";
    std::string mqttPass   = "";
    std::string sensorName = "desk";

    explicit Settings(NvsStorageManager& nvs) : SettingsBase(nvs) {
        field("mqtt_server", mqttServer);
        field("mqtt_port",   mqttPort);
        field("mqtt_user",   mqttUser);
        field("mqtt_pass",   mqttPass);
        field("sensor_name", sensorName);
        load();
    }
};
