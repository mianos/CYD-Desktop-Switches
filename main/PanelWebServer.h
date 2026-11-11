#pragma once

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
    explicit PanelWebServer(WebContext* ctx);

    esp_err_t start() override;

protected:
    void populate_healthz_fields(WebContext* ctx, JsonWrapper& json) override;

private:
    static esp_err_t firmware_get_handler(httpd_req_t* req);
    static esp_err_t firmware_post_handler(httpd_req_t* req);
};
