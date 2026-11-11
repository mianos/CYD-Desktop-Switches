#pragma once

#include <atomic>

#include "Config.h"

class MqttClient;  // from mqttwrapper

namespace ui {

// Hands the UI its (JSON-derived) configuration and the objects it acts on.
// wifiUp / mqttUp are shared flags updated elsewhere; the UI only reads them
// for the status bar. The UI never talks to a specific device: everything it
// publishes comes out of `config`.
void init(const cfg::UiConfig& config, MqttClient* mqtt,
          std::atomic<bool>* wifiUp, std::atomic<bool>* mqttUp);

// Renders the control surface and runs the touch/MQTT loop. Never returns.
void run();

}  // namespace ui
