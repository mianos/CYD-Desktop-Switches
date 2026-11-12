#pragma once

#include <atomic>
#include <string>

#include "esp_err.h"
#include "esp_http_server.h"

#include "JsonWrapper.h"
#include "WebServer.h"

// Desk panel HTTP surface, layered on the shared mianesp WebServer base
// (/reset, /set_hostname, /healthz). It adds the OTA endpoints the other
// mianesp devices expose, so firmware is pushed the same way everywhere:
//   GET  /firmware   running image version / partition
//   POST /firmware   raw .bin body -> inactive OTA slot -> reboot
// Handlers recover this instance from req->user_ctx.
class PanelWebServer : public WebServer {
public:
    // broker/sensor/buttons are reported by /healthz: a panel whose buttons do
    // nothing is almost always pointed at the wrong broker, or still running the
    // built-in placeholder topics, and both are visible in one request.
    PanelWebServer(WebContext* ctx, std::string broker, std::string sensor,
                   std::string buttons, const std::atomic<bool>* wifiUp,
                   const std::atomic<bool>* mqttUp);

    esp_err_t start() override;

protected:
    void populate_healthz_fields(WebContext* ctx, JsonWrapper& json) override;

private:
    static esp_err_t firmware_get_handler(httpd_req_t* req);
    static esp_err_t firmware_post_handler(httpd_req_t* req);

    std::string m_broker;
    std::string m_sensor;
    std::string m_buttons;
    const std::atomic<bool>* m_wifiUp;
    const std::atomic<bool>* m_mqttUp;
};
