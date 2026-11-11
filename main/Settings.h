#pragma once

#include <string>

#include "SettingsBase.h"

// The broker host is site-specific, so it is not committed. Put the real one in
// main/local_settings.h (git-ignored) as
//
//     #define DESK_MQTT_SERVER "mqtt.example.com"
//
// and every build on this machine picks it up. Without that file the compiled-in
// default is empty, so a fresh clone has to be given a broker before it can join.
#if defined(__has_include)
#  if __has_include("local_settings.h")
#    include "local_settings.h"
#  endif
#endif

#ifndef DESK_MQTT_SERVER
#  define DESK_MQTT_SERVER ""
#endif

// Desk-controller settings schema — persistence lives in mianesp's settingsbase.
// Member initialisers are the compiled-in defaults; missing or unparseable NVS
// config falls back to them (logged).
//
// Only the things the panel cannot work without live here: where the broker is
// and what to call itself. Everything the panel *shows and does* is described
// by the JSON config (see Config.h), which is stored separately in NVS so it
// can be replaced over MQTT without touching these fields.
struct Settings : SettingsBase {
    std::string mqttServer = DESK_MQTT_SERVER;  // main/local_settings.h, not in git
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
