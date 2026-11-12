// desk controller — CYD (ESP32-2432S028R) touch panel for the desk.
//
// The panel is generic: what it shows and what it publishes comes from a JSON
// config (main/default_config.json, or a copy replaced at runtime over
// cmnd/<name>/config and cached in NVS) — see Config.h for the schema.
//
// Wi-Fi: ESP-Touch v2 provisioning on first boot (mianesp wifimanager).
// Settings: broker + device name in NVS (mianesp settingsbase), editable at
//           runtime via cmnd/<name>/settings.

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <regex>
#include <string>

#include "Board.h"
#include "Config.h"
#include "Display.h"
#include "JsonWrapper.h"
#include "MqttClient.h"
#include "NvsStorageManager.h"
#include "PanelWebServer.h"
#include "Settings.h"
#include "Ui.h"
#include "WifiManager.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_smartconfig.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "network_provisioning/manager.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace {

const char* TAG = "desk";

std::atomic<bool> s_wifiUp{false};
std::atomic<bool> s_mqttUp{false};

// Given by onIpEvent once the station has an address. The OTA rollback check
// waits on it (otaVerifyTask), so an image that boots but never gets on the
// network is rolled back instead of stranding an unreachable panel.
SemaphoreHandle_t s_got_ip = nullptr;

// Registered for IP events by WiFiManager; drives the UI's Wi-Fi indicator.
void onIpEvent(void*, esp_event_base_t base, int32_t id, void*) {
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    s_wifiUp = true;
    if (s_got_ip) xSemaphoreGive(s_got_ip);
  }
  if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) s_wifiUp = false;
}


// Tracks MQTT connection state for the status bar (mqttwrapper doesn't expose
// it, so we wait on its blocking wait_for_connection()).
void mqttMonitorTask(void* arg) {
  auto* mqtt = static_cast<MqttClient*>(arg);
  while (true) {
    mqtt->wait_for_connection();
    s_mqttUp = true;
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// --- OTA rollback verification ---------------------------------------------
//
// A freshly-OTA'd image boots PENDING_VERIFY: on the next reset the bootloader
// rolls it back unless the running app declares itself good. We declare it good
// only once the panel is back on the network, so an image that boots but cannot
// reach Wi-Fi is rolled back instead of stranding an unreachable panel. A wired
// first flash leaves the state UNDEFINED, so bench boots never touch this.
constexpr int kOtaVerifyTimeoutMs = 120000;

void otaVerifyTask(void*) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    ESP_LOGW(TAG, "OTA: image on %s pending verify; waiting up to %ds for an IP",
             running->label, kOtaVerifyTimeoutMs / 1000);
    if (xSemaphoreTake(s_got_ip, pdMS_TO_TICKS(kOtaVerifyTimeoutMs)) == pdTRUE) {
      esp_ota_mark_app_valid_cancel_rollback();
      ESP_LOGI(TAG, "OTA: connectivity confirmed, image on %s marked valid", running->label);
    } else {
      ESP_LOGE(TAG, "OTA: no IP within timeout; rolling back to the previous image");
      esp_ota_mark_app_invalid_rollback_and_reboot();  // reboots on success
      ESP_LOGE(TAG, "OTA: rollback not possible; keeping the current image");
      esp_ota_mark_app_valid_cancel_rollback();
    }
  }
  vTaskDelete(nullptr);
}

// --- Wall clock -------------------------------------------------------------
//
// Nothing on the glass shows the time, but /healthz and the log lines do, and a
// panel stuck at 1970 makes both useless for correlating with other devices.
// SNTP is started once at boot and keeps retrying until Wi-Fi and DNS are up,
// so there is no ordering to get wrong and nothing to restart when the link
// comes and goes.
//
// The image carries no tzdata, only POSIX TZ strings, so the zone is the one
// line below: AEST/AEDT with the NSW DST rules (first Sunday in October to the
// first Sunday in April). Change it for another site; "UTC0" also works.
constexpr char kTimeZone[] = "AEST-10AEDT,M10.1.0,M4.1.0/3";

void onTimeSynced(struct timeval*) {
  const time_t now = time(nullptr);
  struct tm tm {};
  localtime_r(&now, &tm);
  char buf[32];
  strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
  ESP_LOGI(TAG, "SNTP: clock set to %s (%s)", buf, kTimeZone);
}

void startClock() {
  setenv("TZ", kTimeZone, 1);
  tzset();
  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  cfg.sync_cb = onTimeSynced;
  const esp_err_t err = esp_netif_sntp_init(&cfg);
  if (err != ESP_OK) ESP_LOGW(TAG, "SNTP: init failed: %s", esp_err_to_name(err));
}

// --- Hostname hygiene -------------------------------------------------------
//
// The panel's name is also its Wi-Fi hostname, and both ends of that link are
// stricter than they look:
//
//   * esp_netif_set_hostname() refuses anything over ESP_NETIF_HOSTNAME_MAX_SIZE
//     (32), and wifimanager calls it through ESP_ERROR_CHECK — so a 33-character
//     name is an abort during boot, not a cosmetic problem. sensor_name arrives
//     on cmnd/<name>/settings and is persisted before it is used, so one MQTT
//     message could leave a panel that reboots forever with nothing listening to
//     undo it.
//   * The DHCP request carries the hostname twice (option 12 and ESP-IDF's
//     vendor-class-identifier hook, which falls back to it) plus the client id,
//     so the option area needs 41 + 2*strlen(hostname) bytes. The 69-byte
//     default fits only 14 characters; a longer name associates and then never
//     gets a lease, which this panel answers by wiping the Wi-Fi credentials
//     after 30 minutes (wifiWatchdogTask). sdkconfig.defaults raises the option
//     area to 128 bytes so the 32-character cap below is comfortably inside it.
constexpr size_t kMaxHostname = 32;

std::string clampedHostname(std::string name) {
  if (name.size() > kMaxHostname) {
    ESP_LOGW(TAG, "name '%s' is %u chars; clamping to %u", name.c_str(),
             static_cast<unsigned>(name.size()), static_cast<unsigned>(kMaxHostname));
    name.resize(kMaxHostname);
  }
  return name;
}

// cmnd/<name>/settings — apply + persist a partial settings JSON blob.
esp_err_t handleSettings(MqttClient*, const std::string&, const JsonWrapper& doc, void* ctx) {
  auto* settings = static_cast<Settings*>(ctx);
  settings->loadFromJson(doc);
  // Persisted before the restart that a rename implies, so the stored value can
  // never be one that the next boot cannot apply.
  settings->sensorName = clampedHostname(settings->sensorName);
  settings->save();
  settings->log();
  return ESP_OK;
}

// --- UI config: the panel's behaviour as data -------------------------------
//
// The JSON config describes every button (label, topic, payloads, layout) and
// is what makes this firmware device-agnostic. It lives in NVS under "uicfg"
// and falls back to the copy embedded at build time (main/default_config.json).

constexpr char kUiConfigKey[] = "uicfg";

Settings* g_settings = nullptr;
NvsStorageManager* g_nvs = nullptr;
MqttClient* g_mqtt = nullptr;

// Restart after a short delay so the ack has time to leave the broker. A clean
// boot is the only honest way to apply a new config: mqttwrapper cannot
// unregister a handler, so the old state subscriptions would linger.
void restartTask(void*) {
  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();
}

void publishConfigAck(bool ok, const std::string& detail) {
  if (!g_mqtt || !g_settings) return;
  JsonWrapper ack;
  ack.AddItem("ok", ok);
  if (ok) ack.AddItem("restarting", true);
  else ack.AddItem("err", detail);
  g_mqtt->publish("stat/" + g_settings->sensorName + "/CONFIG", ack.ToString());
}

// cmnd/<name>/config — validate, persist, ack, restart. {"reset":true} drops the
// stored copy so the next boot uses the firmware default again.
esp_err_t handleUiConfig(MqttClient*, const std::string&, const JsonWrapper& doc, void*) {
  bool reset = false;
  doc.GetField("reset", reset);
  if (reset) {
    if (g_nvs) g_nvs->clear(kUiConfigKey);
    ESP_LOGW(TAG, "UI config cleared; restarting into the built-in default");
    publishConfigAck(true, "");
    xTaskCreate(restartTask, "cfg_rst", 2048, nullptr, 2, nullptr);
    return ESP_OK;
  }

  const std::string raw = doc.ToString();
  cfg::UiConfig parsed;
  std::string err;
  if (!cfg::parse(raw, parsed, err)) {
    ESP_LOGE(TAG, "UI config rejected: %s", err.c_str());
    publishConfigAck(false, err);
    return ESP_OK;
  }
  if (!g_nvs || !g_nvs->store(kUiConfigKey, raw)) {
    ESP_LOGE(TAG, "UI config rejected: NVS write failed");
    publishConfigAck(false, "nvs write failed");
    return ESP_OK;
  }

  ESP_LOGI(TAG, "UI config stored (%u rows, %u buttons); restarting to apply",
           (unsigned)parsed.rowHeights.size(), (unsigned)parsed.buttons.size());
  publishConfigAck(true, "");
  xTaskCreate(restartTask, "cfg_rst", 2048, nullptr, 2, nullptr);
  return ESP_OK;
}

// The config to boot with: the NVS copy if it is still valid, else the built-in
// default. A rejected NVS copy is logged and ignored, so a bad update cannot
// brick the panel — the firmware default always works.
cfg::UiConfig loadUiConfig(NvsStorageManager& nvs, const std::string& fallbackTitle) {
  cfg::UiConfig parsed;
  std::string err, raw;
  bool haveStored = nvs.retrieve(kUiConfigKey, raw) && !raw.empty();

  if (haveStored && cfg::parse(raw, parsed, err)) {
    ESP_LOGI(TAG, "UI config from NVS (%u buttons)", (unsigned)parsed.buttons.size());
  } else {
    if (haveStored) ESP_LOGW(TAG, "stored UI config rejected (%s); using the firmware default", err.c_str());
    if (cfg::parse(cfg::defaultJson(), parsed, err))
      ESP_LOGI(TAG, "UI config from firmware default (%u buttons)", (unsigned)parsed.buttons.size());
    else
      ESP_LOGE(TAG, "firmware default config is invalid: %s", err.c_str());
  }

  if (parsed.title.empty()) parsed.title = fallbackTitle;
  return parsed;
}

// "DESK->cmnd/zigbridge/ZbSend, LIGHT->cmnd/light/POWER" — the whole
// publish surface in one line, so /healthz shows at a glance whether the panel
// is aimed at real devices or at the built-in placeholders.
std::string describeConfig(const cfg::UiConfig& config) {
  std::string out;
  for (const auto& b : config.buttons) {
    if (!out.empty()) out += ", ";
    out += b.label + "->" + b.topic;
  }
  return out;
}

// Self-heal stale Wi-Fi credentials. If the device believes it is provisioned but
// never obtains an IP, wipe the record and restart into ESP-Touch so it can be set
// up again. Deliberately patient: a router that is rebooting (or an outage that
// outlasts a few minutes) must not cost us the credentials.
void wifiWatchdogTask(void* arg) {
  auto* wifi = static_cast<WiFiManager*>(arg);
  constexpr int kMinutes = 30;
  bool everUp = false;
  for (int i = 0; i < kMinutes * 2; i++) {  // sample every 30 s
    vTaskDelay(pdMS_TO_TICKS(30000));
    if (s_wifiUp.load()) {
      everUp = true;
      break;
    }
  }
  if (!everUp) {
    bool provisioned = false;
    network_prov_mgr_is_wifi_provisioned(&provisioned);
    if (provisioned) {
      ESP_LOGW(TAG, "no IP %d min after boot; clearing stale Wi-Fi credentials for re-provisioning", kMinutes);
      wifi->clear();  // resets the provisioning record and restarts
    }
  }
  vTaskDelete(nullptr);
}

// Captured in app_main before WiFiManager starts, because its ESP-Touch fallback
// blanks the stored config that esp_wifi_get_config() would otherwise return.
wifi_config_t g_creds = {};
bool g_haveCreds = false;

// Read the credentials the Wi-Fi driver keeps in NVS ("nvs.net80211"). Each is a
// small header followed by the raw bytes (36 = u32 length + 32-byte SSID, 65 =
// u8 length + 64-byte password).
bool readNvsBlob(nvs_handle_t handle, const char* key, uint8_t* out, size_t cap) {
  size_t len = 0;
  if (nvs_get_blob(handle, key, nullptr, &len) != ESP_OK) return false;
  if (len < 5 || len > cap) return false;
  return nvs_get_blob(handle, key, out, &len) == ESP_OK;
}

bool loadStoredWifiCreds(wifi_config_t& cfg) {
  nvs_handle_t handle;
  if (nvs_open("nvs.net80211", NVS_READONLY, &handle) != ESP_OK) return false;
  uint8_t ssid[68] = {};
  uint8_t pswd[68] = {};
  const bool haveSsid = readNvsBlob(handle, "sta.ssid", ssid, sizeof ssid);
  const bool havePswd = readNvsBlob(handle, "sta.pswd", pswd, sizeof pswd);
  nvs_close(handle);
  if (!haveSsid) return false;

  uint32_t ssidLen = 0;
  memcpy(&ssidLen, ssid, sizeof ssidLen);  // 36-byte blob: u32 length + 32-byte SSID
  if (ssidLen == 0 || ssidLen > 32) return false;
  // 65-byte blob holding a NUL-terminated password (no length prefix).
  const size_t pswdLen = havePswd ? strnlen(reinterpret_cast<char*>(pswd), 64) : 0;

  memset(&cfg, 0, sizeof cfg);
  memcpy(cfg.sta.ssid, ssid + sizeof ssidLen, ssidLen);
  if (pswdLen) memcpy(cfg.sta.password, pswd, pswdLen);
  cfg.sta.threshold.authmode = pswdLen ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  return cfg.sta.ssid[0] != '\0';
}

// Note on Wi-Fi bring-up: wifimanager keys its "already provisioned" check on the
// network_provisioning record, but SmartConfig/ESP-Touch only stores the SSID and
// password in the Wi-Fi NVS config and never writes that record. So after a
// successful ESP-Touch the next boot takes the "not provisioned" branch and starts
// ESP-Touch again. ESP-Touch puts the radio into sniffer mode, which drops the
// station link: waiting for an IP before stopping it (the old approach) is a race
// the link usually loses, leaving the board associated-but-dead with valid
// credentials in NVS. Instead, restore the stored credentials from NVS up front,
// keep ESP-Touch stopped and re-issue esp_wifi_connect() until an IP arrives. With
// no stored credentials we leave ESP-Touch alone so first-time provisioning works.
void wifiBootstrapTask(void*) {
  ESP_LOGI(TAG, "stored SSID: %s", g_haveCreds ? reinterpret_cast<char*>(g_creds.sta.ssid) : "(none)");

  bool applied = false;
  bool stopped = false;
  int attempts = 0;
  while (true) {
    if (s_wifiUp.load()) {
      if (!stopped) {
        stopped = true;
        if (esp_smartconfig_stop() == ESP_OK) ESP_LOGI(TAG, "SmartConfig stopped; link is up");
      }
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    if (!g_haveCreds) {  // genuinely unprovisioned: let ESP-Touch run
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    if (!applied) {
      // ESP-Touch put the radio in sniffer mode and cleared the driver's stored
      // config, so stop it and re-apply the credentials we captured at boot.
      esp_smartconfig_stop();  // errors until smartConfigTask has started it
      applied = esp_wifi_set_config(WIFI_IF_STA, &g_creds) == ESP_OK;
      if (applied)
        ESP_LOGI(TAG, "restored stored credentials for '%s'", reinterpret_cast<char*>(g_creds.sta.ssid));
    }

    // Only kick the association if we are not on an AP yet: calling
    // esp_wifi_connect() while associated restarts DHCP and the board never
    // picks up an address.
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
      if (attempts++ % 5 == 0) ESP_LOGI(TAG, "no IP; reconnecting to '%s'", reinterpret_cast<char*>(g_creds.sta.ssid));
      esp_wifi_connect();
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

}  // namespace

extern "C" void app_main(void) {
  display::init();
  display::setBrightness(CONFIG_CYD_BRIGHTNESS);

  static NvsStorageManager nvs;  // constructing this initialises NVS flash
  static Settings settings(nvs);
  // Clamp before anything reads it: the name feeds the hostname, the MQTT client
  // id and every topic, and a stored value past 32 characters would abort the
  // boot (see clampedHostname). An NVS copy that is already too long therefore
  // heals itself here rather than bricking the panel.
  settings.sensorName = clampedHostname(settings.sensorName);
  settings.log();

  // Capture the stored Wi-Fi credentials before WiFiManager can start its
  // ESP-Touch fallback; starting ESP-Touch clears the driver's persisted config.
  g_haveCreds = loadStoredWifiCreds(g_creds);

  // WiFiManager's constructor loads this same record and applies it through
  // ESP_ERROR_CHECK, before app_main gets a say — so POST /set_hostname (which is
  // not length-checked) has to be sanitised while it is still only in NVS.
  {
    std::string stored;
    if (nvs.retrieve("hostname", stored) && stored.size() > kMaxHostname) {
      ESP_LOGW(TAG, "stored hostname is %u chars; rewriting it as '%s'",
               static_cast<unsigned>(stored.size()), settings.sensorName.c_str());
      nvs.store("hostname", settings.sensorName);
    }
  }

  s_got_ip = xSemaphoreCreateBinary();

  // Wi-Fi: ESP-Touch v2 on first boot, else reconnect (wifimanager).
  static WiFiManager wifi(nvs, onIpEvent, nullptr);
  std::string host = settings.sensorName;
  wifi.configSetHostName(host);

  // MQTT. An NVS copy that lost its broker (an empty-string save, a wiped
  // partition) would leave the panel looking healthy but permanently silent, so
  // fall back to the compiled host rather than trusting the empty value.
  if (settings.mqttServer.empty() && *DESK_MQTT_SERVER != '\0') {
    settings.mqttServer = DESK_MQTT_SERVER;
    ESP_LOGW(TAG, "mqtt_server empty in NVS; using compiled default '%s'",
             settings.mqttServer.c_str());
  }
  static std::string uri = "mqtt://" + settings.mqttServer + ":" + std::to_string(settings.mqttPort);
  static esp_mqtt_client_config_t mcfg = {};
  mcfg.broker.address.uri = uri.c_str();
  mcfg.credentials.client_id = settings.sensorName.c_str();
  if (!settings.mqttUser.empty()) {
    mcfg.credentials.username = settings.mqttUser.c_str();
    mcfg.credentials.authentication.password = settings.mqttPass.c_str();
  }
  mcfg.session.keepalive = 30;
  // The UI config arrives as one JSON blob on cmnd/<name>/config — 1.1 KB for the
  // built-in layout before any state/query sections are added, which is already
  // past esp-mqtt's 1024-byte default. Oversized inbound messages are dropped
  // before they reach a handler, so the panel would look perfectly connected and
  // still ignore every config push. Give the client room for the whole config.
  mcfg.buffer.size = 4096;
  static MqttClient mqtt(mcfg, settings.sensorName);
  g_settings = &settings;
  g_nvs = &nvs;
  g_mqtt = &mqtt;

  const std::string base = "cmnd/" + settings.sensorName + "/";
  mqtt.registerHandler(base + "settings", std::regex(base + "settings"), handleSettings, &settings);
  mqtt.registerHandler(base + "config", std::regex(base + "config"), handleUiConfig, nullptr);
  mqtt.start();

  xTaskCreate(mqttMonitorTask, "mqtt_mon", 4096, &mqtt, 5, nullptr);
  xTaskCreate(wifiWatchdogTask, "wifi_wd", 4096, &wifi, 3, nullptr);
  xTaskCreate(wifiBootstrapTask, "wifi_boot", 4096, nullptr, 3, nullptr);

  // HTTP surface: /healthz, /reset, /set_hostname from the shared mianesp
  // WebServer base, plus OTA (/firmware) — the same routes stillerate2,
  // ws-voice and mqttradar expose, so ./build.sh ota works here too.
  startClock();
  // Loaded before the web server starts so /healthz can report the live config.
  const cfg::UiConfig uiCfg = loadUiConfig(nvs, settings.sensorName);
  static WebContext webctx(&wifi);
  static PanelWebServer web(&webctx, settings.mqttServer, settings.sensorName,
                            describeConfig(uiCfg), &s_wifiUp, &s_mqttUp);
  if (web.start() != ESP_OK) ESP_LOGE(TAG, "web server failed to start");
  ESP_LOGI(TAG, "OTA ready at http://<ip>/firmware (heap free %u)",
           static_cast<unsigned>(esp_get_free_heap_size()));
  xTaskCreate(otaVerifyTask, "ota_verify", 4096, nullptr, 4, nullptr);

  ui::init(uiCfg, &mqtt, &s_wifiUp, &s_mqttUp);

  ESP_LOGI(TAG, "desk controller started");

  ui::run();  // never returns
}
