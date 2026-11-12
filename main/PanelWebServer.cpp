#include "PanelWebServer.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <ctime>
#include <string>
#include <utility>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char* TAG = "panelweb";

esp_err_t send_json(httpd_req_t* req, const JsonWrapper& json) {
    httpd_resp_set_type(req, "application/json");
    std::string out = json.ToString();
    return httpd_resp_sendstr(req, out.c_str());
}

}  // namespace

PanelWebServer::PanelWebServer(WebContext* ctx, std::string broker, std::string sensor,
                              std::string buttons, const std::atomic<bool>* wifiUp,
                              const std::atomic<bool>* mqttUp)
    : WebServer(ctx),
      m_broker(std::move(broker)),
      m_sensor(std::move(sensor)),
      m_buttons(std::move(buttons)),
      m_wifiUp(wifiUp),
      m_mqttUp(mqttUp) {}

esp_err_t PanelWebServer::start() {
    esp_err_t r = WebServer::start();
    if (r != ESP_OK) return r;

    struct Route {
        const char*    uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t*);
    };
    const std::array<Route, 2> routes = {{
        {"/firmware", HTTP_POST, firmware_post_handler},
        {"/firmware", HTTP_GET,  firmware_get_handler},
    }};

    for (const Route& route : routes) {
        httpd_uri_t uri = {
            .uri      = route.uri,
            .method   = route.method,
            .handler  = route.handler,
            .user_ctx = this,
        };
        esp_err_t err = httpd_register_uri_handler(server, &uri);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s %s: %s",
                route.method == HTTP_POST ? "POST" : "GET", route.uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

void PanelWebServer::populate_healthz_fields(WebContext*, JsonWrapper& json) {
    const esp_app_desc_t*  desc    = esp_app_get_description();
    const esp_partition_t* running = esp_ota_get_running_partition();
    json.AddItem("version",   std::string(desc->version));
    json.AddItem("partition", std::string(running->label));
    json.AddItem("heap_free", static_cast<int>(esp_get_free_heap_size()));
    // Anything earlier than 2020-09 means SNTP has not landed yet and the
    // "time" field is still the 1970 default.
    json.AddItem("clock_set", static_cast<bool>(time(nullptr) > 1600000000));
    // Where the panel is and what it drives. "mqtt" is true once the client has
    // connected since boot (mqttwrapper exposes no live state), and "buttons"
    // lists the topics a tap publishes to — the quickest way to spot a panel
    // still running the placeholder config, or configured for another broker.
    json.AddItem("wifi",   static_cast<bool>(m_wifiUp && m_wifiUp->load()));
    json.AddItem("mqtt",   static_cast<bool>(m_mqttUp && m_mqttUp->load()));
    json.AddItem("broker", m_broker);
    json.AddItem("sensor", m_sensor);
    json.AddItem("config_topic", "cmnd/" + m_sensor + "/config");
    json.AddItem("buttons", m_buttons);
}

// POST /firmware — raw .bin body. Streams into the inactive OTA slot, sets it as
// the next boot partition, then reboots.
// Deploy: curl -H 'Expect:' --data-binary @build/cyd_display_test.bin http://<ip>/firmware
esp_err_t PanelWebServer::firmware_post_handler(httpd_req_t* req) {
    if (req->content_len <= 0) return sendJsonError(req, 400, "Content-Length required");

    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr) return sendJsonError(req, 500, "no OTA partition available");
    if (req->content_len > static_cast<int>(target->size)) {
        return sendJsonError(req, 413, "image larger than OTA partition");
    }
    ESP_LOGI(TAG, "OTA: writing %d bytes to %s @ 0x%" PRIx32,
        req->content_len, target->label, target->address);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) return sendJsonError(req, 500, esp_err_to_name(err));

    // Sized to the smallest TCP window: the download is a straight copy into
    // flash, so a small buffer costs one recv() per 1 KB and no extra RAM.
    char buf[1024];
    int remaining = req->content_len;
    int written = 0;
    while (remaining > 0) {
        int got = httpd_req_recv(req, buf, std::min<int>(remaining, static_cast<int>(sizeof buf)));
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) {
            esp_ota_abort(handle);
            return sendJsonError(req, 400, "request body truncated");
        }
        err = esp_ota_write(handle, buf, got);
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            return sendJsonError(req, 500, esp_err_to_name(err));
        }
        written   += got;
        remaining -= got;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) return sendJsonError(req, 400, esp_err_to_name(err));
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) return sendJsonError(req, 500, esp_err_to_name(err));

    ESP_LOGW(TAG, "OTA: %d bytes written to %s; rebooting", written, target->label);
    JsonWrapper resp;
    resp.AddItem("status",    std::string("ok"));
    resp.AddItem("written",   written);
    resp.AddItem("partition", std::string(target->label));
    send_json(req, resp);

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t PanelWebServer::firmware_get_handler(httpd_req_t* req) {
    const esp_app_desc_t*  desc    = esp_app_get_description();
    const esp_partition_t* running = esp_ota_get_running_partition();
    JsonWrapper resp;
    resp.AddItem("version",   std::string(desc->version));
    resp.AddItem("idf_ver",   std::string(desc->idf_ver));
    resp.AddItem("date",      std::string(desc->date));
    resp.AddItem("time",      std::string(desc->time));
    resp.AddItem("partition", std::string(running->label));
    return send_json(req, resp);
}
