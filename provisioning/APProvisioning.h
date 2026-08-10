#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "../core/Config.h"
#include "../core/Identity.h"
#include "../provisioning/ProvisioningManager.h"
#include "../security/CredentialStore.h"

class APProvisioning {
public:
    static void start() {
        String apSSID = String(AP_SSID) + "-" + Identity::get().substring(4, 10);
        String apPass = CredentialStore::apPassword();

        WiFi.mode(WIFI_AP_STA);
        WiFi.softAP(apSSID.c_str(), apPass.c_str());
        LOG_I("AP", "AP started — SSID: %s", apSSID.c_str());
        LOG_I("AP", "AP IP: %s", WiFi.softAPIP().toString().c_str());

        _server = new WebServer(AP_PORT);
        _setupRoutes();
        _server->begin();
        _active  = true;
        _success = false;
        ProvisioningManager::setActiveMode(ProvMode::AP);
    }

    static void stop() {
        if (_server) { _server->stop(); delete _server; _server = nullptr; }
        WiFi.softAPdisconnect(true);
        _active = false;
        ProvisioningManager::setActiveMode(ProvMode::NONE);
        LOG_I("AP", "AP provisioning stopped");
    }

    static bool tick() {
        if (_active && _server) _server->handleClient();
        return _success;
    }

    static bool isActive()  { return _active; }
    static bool isSuccess() { return _success; }

private:
    static void _setupRoutes() {
        _server->on("/",         HTTP_GET,  _handleRoot);
        _server->on("/save",     HTTP_POST, _handleSave);
        _server->on("/status",   HTTP_GET,  _handleStatus);
        _server->onNotFound([]() {
            // Captive portal redirect
            _server->sendHeader("Location", "http://192.168.4.1/");
            _server->send(302, "text/plain", "");
        });
    }

    static void _handleRoot() {
        _server->send(200, "text/html", _buildPage());
    }

    static void _handleSave() {
        String ssid      = _server->arg("wifi_ssid");
        String pass      = _server->arg("wifi_pass");
        String mqttUser  = _server->arg("mqtt_user");
        String mqttPass  = _server->arg("mqtt_pass");
        String devSecret = _server->arg("dev_secret");
        String url       = _server->arg("backend_url");

        if (ssid.length() == 0 || mqttUser.length() == 0) {
            _server->send(400, "application/json", "{\"error\":\"Missing required fields\"}");
            return;
        }

        bool ok = ProvisioningManager::commitCredentials(ssid, pass, mqttUser, mqttPass, devSecret, url);
        if (ok) {
            _server->send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Device provisioned\"}");
            _success = true;
            LOG_I("AP", "AP provisioning successful");
        } else {
            _server->send(500, "application/json", "{\"error\":\"Commit failed\"}");
        }
    }

    static void _handleStatus() {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"unit_id\":\"%s\",\"fw\":\"%s\",\"provisioned\":%s}",
                 Identity::get().c_str(), FIRMWARE_VERSION,
                 CredentialStore::hasWiFiCredentials() ? "true" : "false");
        _server->send(200, "application/json", buf);
    }

    static String _buildPage() {
        String html = F("<!DOCTYPE html><html><head>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>LumaRoK Setup</title>"
            "<style>body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:20px}"
            "input,button{width:100%;padding:10px;margin:8px 0;box-sizing:border-box}"
            "button{background:#2563eb;color:#fff;border:none;border-radius:6px;cursor:pointer}"
            "</style></head><body>"
            "<h2>LumaRoK Device Setup</h2>"
            "<p>Unit: ");
        html += Identity::get();
        html += F("</p>"
            "<form method='POST' action='/save'>"
            "<input name='wifi_ssid'   placeholder='WiFi SSID'      required>"
            "<input name='wifi_pass'   placeholder='WiFi Password'   type='password'>"
            "<input name='mqtt_user'   placeholder='MQTT Username'   required>"
            "<input name='mqtt_pass'   placeholder='MQTT Password'   type='password' required>"
            "<input name='dev_secret'  placeholder='Device Secret'   required>"
            "<input name='backend_url' placeholder='Backend URL (https://...)'>"
            "<button type='submit'>Save &amp; Connect</button>"
            "</form></body></html>");
        return html;
    }

    static WebServer* _server;
    static bool       _active;
    static bool       _success;
};

inline WebServer* APProvisioning::_server  = nullptr;
inline bool       APProvisioning::_active  = false;
inline bool       APProvisioning::_success = false;
