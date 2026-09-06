/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi Configuration and SoftAP support for ESP Thread Border Router
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_br_web.h"
#include "esp_br_wifi_config.h"
#include "esp_br_wifi_config_handlers.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_ot_wifi_cmd.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/ip_addr.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#define WIFI_CONFIG_TAG "wifi_config"
#define WIFI_CONFIGURED_BIT BIT1
#define DNS_PORT 53
#define MAX_SSID_LEN 32
#define MAX_PASSWORD_LEN 64
#define MAX_SCAN_RESULTS 24

static bool s_wifi_config_mode = false;
static bool s_wifi_stack_owned = false;
static esp_netif_t *s_ap_netif = NULL;
static EventGroupHandle_t s_wifi_event_group = NULL;
static TaskHandle_t s_dns_task_handle = NULL;
static SemaphoreHandle_t s_dns_task_semaphore = NULL;
static int s_dns_socket = -1;
static esp_event_handler_instance_t s_wifi_event_handler_instance = NULL;
static char s_softap_ssid[32] = "";
static char s_configured_ssid[32] = "";
static char s_configured_password[64] = "";

static esp_err_t wifi_config_dns_server_start(void);
static void wifi_config_dns_server_stop(void);
static void wifi_config_dns_server_task(void *arg);
static esp_err_t wifi_config_start_softap(void);
static void wifi_config_stop_softap(void);
static void wifi_config_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

// DNS Server implementation
static esp_err_t wifi_config_dns_server_start(void)
{
    esp_err_t ret = ESP_OK;
    s_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_GOTO_ON_FALSE(s_dns_socket >= 0, ESP_FAIL, exit, WIFI_CONFIG_TAG, "Failed to create DNS socket");

    // Set socket to reuse address
    int opt = 1;
    setsockopt(s_dns_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Set socket to non-blocking mode
    int flags = fcntl(s_dns_socket, F_GETFL, 0);
    fcntl(s_dns_socket, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(DNS_PORT);

    ESP_GOTO_ON_FALSE(bind(s_dns_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) >= 0, ESP_FAIL, exit,
                      WIFI_CONFIG_TAG, "Failed to bind DNS port %d", DNS_PORT);

    // Create semaphore for task synchronization
    s_dns_task_semaphore = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(s_dns_task_semaphore != NULL, ESP_ERR_NO_MEM, exit, WIFI_CONFIG_TAG,
                      "Failed to create DNS task semaphore");
    ESP_GOTO_ON_FALSE(xTaskCreate(wifi_config_dns_server_task, "dns_server", 4096, NULL, 5, &s_dns_task_handle) ==
                          pdPASS,
                      ESP_ERR_NO_MEM, exit, WIFI_CONFIG_TAG, "Failed to create DNS server task");

    ESP_LOGI(WIFI_CONFIG_TAG, "DNS server started");
    return ESP_OK;

exit:
    if (s_dns_task_semaphore) {
        vSemaphoreDelete(s_dns_task_semaphore);
        s_dns_task_semaphore = NULL;
    }
    if (s_dns_socket >= 0) {
        close(s_dns_socket);
        s_dns_socket = -1;
    }
    return ret;
}

static void wifi_config_dns_server_stop(void)
{
    // Close socket first to signal task to exit
    if (s_dns_socket >= 0) {
        close(s_dns_socket);
        s_dns_socket = -1;
    }

    if (s_dns_task_handle && s_dns_task_semaphore) {
        TaskHandle_t task_handle = s_dns_task_handle;
        // Wait for task to signal completion via semaphore (task will exit when it detects socket is closed)
        if (xSemaphoreTake(s_dns_task_semaphore, pdMS_TO_TICKS(1000)) != pdTRUE) {
            // Timeout: task didn't exit in time, force delete
            ESP_LOGW(WIFI_CONFIG_TAG, "DNS task did not exit in time, forcing deletion");
            if (s_dns_task_handle) {
                vTaskDelete(task_handle);
                s_dns_task_handle = NULL;
            }
        }
    }

    // Clean up semaphore
    if (s_dns_task_semaphore) {
        vSemaphoreDelete(s_dns_task_semaphore);
        s_dns_task_semaphore = NULL;
    }

    ESP_LOGI(WIFI_CONFIG_TAG, "DNS server stopped");
}

static void wifi_config_dns_server_task(void *arg)
{
    // Store gateway IP address locally to avoid accessing potentially invalid pointer
    esp_ip4_addr_t gateway_addr;
    IP4_ADDR(&gateway_addr, 192, 168, 4, 1);

    char buffer[512];

    while (s_dns_task_handle != NULL) {
        if (s_dns_socket < 0) {
            break;
        }

        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int len = recvfrom(s_dns_socket, buffer, sizeof(buffer), 0, (struct sockaddr *)&client_addr, &client_addr_len);
        if (len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGE(WIFI_CONFIG_TAG, "DNS recvfrom failed, errno=%d", errno);
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Check minimum DNS query length (12 bytes header)
        if (len < 12) {
            continue;
        }

        // Check buffer size to prevent overflow
        if (len + 16 > sizeof(buffer)) {
            continue;
        }

        // Simple DNS response: point all queries to gateway IP
        buffer[2] |= 0x80; // Set response flag (QR = 1)
        buffer[3] |= 0x80; // Set Recursion Available (RA = 1)
        buffer[7] = 1;     // Set answer count to 1

        // Add answer section
        memcpy(&buffer[len], "\xc0\x0c", 2); // Name pointer to question name
        len += 2;
        memcpy(&buffer[len], "\x00\x01\x00\x01\x00\x00\x00\x1c\x00\x04", 10); // Type A, class IN, TTL 28, data length 4
        len += 10;
        memcpy(&buffer[len], &gateway_addr.addr, 4); // Gateway IP
        len += 4;

        int sent = sendto(s_dns_socket, buffer, len, 0, (struct sockaddr *)&client_addr, client_addr_len);
        if (sent < 0) {
            ESP_LOGE(WIFI_CONFIG_TAG, "DNS sendto failed, errno=%d", errno);
        }
    }
    // Signal completion via semaphore before clearing task handle
    if (s_dns_task_semaphore) {
        xSemaphoreGive(s_dns_task_semaphore);
    }
    // Clear task handle before exiting to signal completion
    s_dns_task_handle = NULL;
    vTaskDelete(NULL);
}

// SoftAP implementation

/** @brief Whether the Wi-Fi driver has already been initialized by another module. */
static bool wifi_stack_is_initialized(void)
{
    wifi_mode_t mode;
    return esp_wifi_get_mode(&mode) != ESP_ERR_WIFI_NOT_INIT;
}

static esp_err_t wifi_config_start_softap(void)
{
    esp_err_t ret = ESP_OK;
    uint8_t mac[6];

    // The Wi-Fi driver may already be up: the station side is initialized before the SoftAP is
    // started whenever a stored Wi-Fi configuration was tried first. Only initialize (and later
    // deinitialize) the driver when this module is the one that brought it up.
    if (!wifi_stack_is_initialized()) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), WIFI_CONFIG_TAG, "Failed to initialize Wi-Fi");
        s_wifi_stack_owned = true;
        // Keep the AP credentials out of NVS so they cannot interfere with the station config.
        ESP_GOTO_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), cleanup, WIFI_CONFIG_TAG,
                          "Failed to set Wi-Fi storage");
    }

    // Create AP netif
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_GOTO_ON_FALSE(s_ap_netif != NULL, ESP_FAIL, cleanup, WIFI_CONFIG_TAG, "Failed to create AP netif");

    // Set AP IP address to 192.168.4.1
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    esp_netif_dhcps_stop(s_ap_netif);
    ESP_GOTO_ON_ERROR(esp_netif_set_ip_info(s_ap_netif, &ip_info), cleanup, WIFI_CONFIG_TAG,
                      "Failed to set AP IP info");
    ESP_GOTO_ON_ERROR(esp_netif_dhcps_start(s_ap_netif), cleanup, WIFI_CONFIG_TAG, "Failed to start AP DHCP server");

    // Get MAC address for SSID
    ESP_GOTO_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP), cleanup, WIFI_CONFIG_TAG, "Failed to read Wi-Fi MAC");

    // Generate SSID: ESP-ThreadBR-XXXX
    snprintf(s_softap_ssid, sizeof(s_softap_ssid), "ESP-ThreadBR-%02X%02X", mac[4], mac[5]);

    // Configure WiFi AP
    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.ap.ssid, s_softap_ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid[sizeof(wifi_config.ap.ssid) - 1] = '\0';
    wifi_config.ap.ssid_len = strlen(s_softap_ssid);
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    wifi_config.ap.beacon_interval = 100; // Set beacon interval to 100ms for better discoverability

    // Register event handlers
    ESP_GOTO_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                          wifi_config_wifi_event_handler, NULL,
                                                          &s_wifi_event_handler_instance),
                      cleanup, WIFI_CONFIG_TAG, "Failed to register Wi-Fi event handler");

    // Set WiFi mode and start AP. APSTA keeps the station interface available for scanning and,
    // when the station side was already running, for its ongoing reconnect attempts.
    ESP_GOTO_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), cleanup, WIFI_CONFIG_TAG, "Failed to set APSTA mode");
    ESP_GOTO_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), cleanup, WIFI_CONFIG_TAG,
                      "Failed to set AP config");
    ESP_GOTO_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), cleanup, WIFI_CONFIG_TAG, "Failed to disable Wi-Fi power save");
    ESP_GOTO_ON_ERROR(esp_wifi_start(), cleanup, WIFI_CONFIG_TAG, "Failed to start Wi-Fi");

    // Start DNS server for the captive portal
    wifi_config_dns_server_start();

    ESP_LOGI(WIFI_CONFIG_TAG, "SoftAP started with SSID: %s", s_softap_ssid);

    return ESP_OK;

cleanup:
    wifi_config_stop_softap();
    return ret;
}

static void wifi_config_stop_softap(void)
{
    // Stop DNS server first (this may take some time)
    wifi_config_dns_server_stop();

    // Unregister event handlers before touching the Wi-Fi driver
    if (s_wifi_event_handler_instance) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_event_handler_instance);
        s_wifi_event_handler_instance = NULL;
    }

    if (wifi_stack_is_initialized()) {
        // Drop the AP interface but keep the station side of the driver running, so that whoever
        // owns the station connection is not left with a deinitialized driver.
        esp_err_t err = esp_wifi_set_mode(s_wifi_stack_owned ? WIFI_MODE_NULL : WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGW(WIFI_CONFIG_TAG, "Failed to leave AP mode: %s", esp_err_to_name(err));
        }
    }

    // The AP netif must only be destroyed once the Wi-Fi driver no longer serves it: lwIP still
    // transmits (IGMP leave, mDNS goodbye) while the interface is being removed, and doing that
    // through a detached driver dereferences a null transmit callback.
    if (s_wifi_stack_owned) {
        esp_wifi_stop();
    }

    if (s_ap_netif) {
        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }

    if (s_wifi_stack_owned) {
        esp_wifi_deinit();
        s_wifi_stack_owned = false;
    }

    ESP_LOGI(WIFI_CONFIG_TAG, "SoftAP stopped");
}

// WiFi event handlers
static void wifi_config_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id) {
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(WIFI_CONFIG_TAG, "Station " MACSTR " joined, AID=%d", MAC2STR(event->mac), event->aid);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(WIFI_CONFIG_TAG, "Station " MACSTR " left, AID=%d", MAC2STR(event->mac), event->aid);
        break;
    }
    default:
        // Unhandled event, ignore
        break;
    }
}

// HTTP handlers for WiFi configuration. They are registered on the border router web server so
// that a single HTTP server serves both the provisioning portal and the Thread web GUI.

esp_err_t esp_br_wifi_config_scan_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    if (!wifi_stack_is_initialized()) {
        httpd_resp_send(req, "{\"aps\":[]}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    wifi_scan_config_t scan_config = {.ssid = NULL,
                                      .bssid = NULL,
                                      .channel = 0,
                                      .show_hidden = false,
                                      .scan_type = WIFI_SCAN_TYPE_ACTIVE,
                                      .scan_time = {.active = {.min = 100, .max = 300}}};

    // Blocking scan: the calling HTTP task waits for the result, so no scan state has to be kept.
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGE(WIFI_CONFIG_TAG, "Failed to scan: %s", esp_err_to_name(ret));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "{\"error\":\"Failed to scan\"}", HTTPD_RESP_USE_STRLEN);
        return ret;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > MAX_SCAN_RESULTS) {
        ap_count = MAX_SCAN_RESULTS;
    }

    wifi_ap_record_t *records = NULL;
    if (ap_count > 0) {
        records = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (!records) {
            esp_wifi_clear_ap_list();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate memory");
            return ESP_ERR_NO_MEM;
        }
        esp_wifi_scan_get_ap_records(&ap_count, records);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *aps = cJSON_CreateArray();
    for (int i = 0; i < ap_count; i++) {
        cJSON *ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", (char *)records[i].ssid);
        cJSON_AddNumberToObject(ap, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(ap, "authmode", records[i].authmode);
        cJSON_AddItemToArray(aps, ap);
    }
    free(records);
    cJSON_AddItemToObject(root, "aps", aps);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate memory");
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_send(req, json_str, strlen(json_str));
    cJSON_free(json_str);
    return ESP_OK;
}

/** @brief Reboot shortly after a response has been flushed to the client. */
static void wifi_config_delayed_restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

esp_err_t esp_br_wifi_config_submit_post_handler(httpd_req_t *req)
{
    char *buf = NULL;
    size_t buf_len = req->content_len;

    if (buf_len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_ERR_INVALID_ARG;
    }

    buf = malloc(buf_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate memory");
        return ESP_ERR_NO_MEM;
    }

    int recv_len = httpd_req_recv(req, buf, buf_len);
    if (recv_len <= 0) {
        free(buf);
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
            return ESP_ERR_TIMEOUT;
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive request");
            return ESP_FAIL;
        }
    }
    buf[recv_len] = '\0';

    // Parse JSON
    cJSON *json = cJSON_Parse(buf);
    free(buf);
    buf = NULL;
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    cJSON *password_item = cJSON_GetObjectItemCaseSensitive(json, "password");

    if (!cJSON_IsString(ssid_item) || !ssid_item->valuestring || strlen(ssid_item->valuestring) >= MAX_SSID_LEN + 1) {
        cJSON_Delete(json);
        httpd_resp_send(req, "{\"success\":false,\"error\":\"Invalid SSID\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    const char *ssid = ssid_item->valuestring;
    const char *password = NULL;
    if (cJSON_IsString(password_item) && password_item->valuestring &&
        strlen(password_item->valuestring) < MAX_PASSWORD_LEN + 1) {
        password = password_item->valuestring;
    }

    // Store configured WiFi credentials in memory (will be saved to NVS by caller)
    strncpy(s_configured_ssid, ssid, sizeof(s_configured_ssid) - 1);
    s_configured_ssid[sizeof(s_configured_ssid) - 1] = '\0';
    if (password && strlen(password) > 0) {
        strncpy(s_configured_password, password, sizeof(s_configured_password) - 1);
        s_configured_password[sizeof(s_configured_password) - 1] = '\0';
    } else {
        s_configured_password[0] = '\0';
    }

    ESP_LOGI(WIFI_CONFIG_TAG, "WiFi configuration received: SSID=%s", s_configured_ssid);
    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/json");

    // Persist the credentials right away and apply them with a restart. If they turn out to be
    // wrong, the border router falls back to the provisioning SoftAP on the next boot.
    esp_err_t err = esp_ot_wifi_config_set_ssid(s_configured_ssid);
    if (err == ESP_OK) {
        err = esp_ot_wifi_config_set_password(s_configured_password);
    }
    if (err != ESP_OK) {
        ESP_LOGE(WIFI_CONFIG_TAG, "Failed to store Wi-Fi credentials: %s", esp_err_to_name(err));
        httpd_resp_send(req, "{\"success\":false,\"error\":\"Failed to store credentials\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_send(req, "{\"success\":true,\"restart\":true}", HTTPD_RESP_USE_STRLEN);

    if (s_wifi_config_mode) {
        // Provisioning mode: the border router startup sequence owns the restart.
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONFIGURED_BIT);
        }
        return ESP_OK;
    }

    if (xTaskCreate(wifi_config_delayed_restart_task, "wifi_restart", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(WIFI_CONFIG_TAG, "Failed to create restart task");
    }
    return ESP_OK;
}

static const char *wifi_state_to_string(esp_ot_wifi_state_t state)
{
    switch (state) {
    case OT_WIFI_CONNECTED:
        return "connected";
    case OT_WIFI_RECONNECTING:
        return "reconnecting";
    default:
        return "disconnected";
    }
}

esp_err_t esp_br_wifi_config_status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate memory");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "provisioning", s_wifi_config_mode);
    cJSON_AddStringToObject(root, "softap_ssid", s_wifi_config_mode ? s_softap_ssid : "");

    char stored_ssid[MAX_SSID_LEN + 1] = "";
    if (esp_ot_wifi_config_get_ssid(stored_ssid) != ESP_OK) {
        stored_ssid[0] = '\0';
    }
    cJSON_AddStringToObject(root, "configured_ssid", stored_ssid);
    cJSON_AddStringToObject(root, "state", wifi_state_to_string(esp_ot_wifi_state_get()));

    char ip[16] = "";
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_info.ip));
    }
    cJSON_AddStringToObject(root, "ip", ip);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate memory");
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    cJSON_free(json_str);
    return ESP_OK;
}

esp_err_t esp_br_wifi_config_icon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Captive portal handler
esp_err_t esp_br_wifi_config_captive_portal_get_handler(httpd_req_t *req)
{
    if (!s_wifi_config_mode) {
        // No portal to advertise once the device runs as a station: report "internet reachable"
        // instead of hijacking the connectivity check.
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/wifi_configuration.html");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t esp_br_wifi_config_start(void)
{
    ESP_RETURN_ON_FALSE(!s_wifi_config_mode, ESP_OK, WIFI_CONFIG_TAG, "WiFi config mode already started");

    esp_err_t ret = ESP_OK;

    // Create event group (for WiFi connection status if needed)
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
        ESP_GOTO_ON_FALSE(s_wifi_event_group != NULL, ESP_ERR_NO_MEM, cleanup, WIFI_CONFIG_TAG,
                          "Failed to create event group");
    }

    // Start SoftAP. The web server itself is shared with the Thread border router GUI and is
    // already running at this point, so the provisioning portal is reachable immediately.
    ESP_GOTO_ON_ERROR(wifi_config_start_softap(), cleanup, WIFI_CONFIG_TAG, "Failed to start SoftAP");

    s_wifi_config_mode = true;
    ESP_LOGI(WIFI_CONFIG_TAG, "WiFi configuration mode started");
    ESP_LOGI(WIFI_CONFIG_TAG, "Access web interface at: http://192.168.4.1");

    return ESP_OK;

cleanup:
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
    return ret;
}

esp_err_t esp_br_wifi_config_stop(void)
{
    if (!s_wifi_config_mode) {
        return ESP_OK;
    }

    // Clear the mode flag first: HTTP handlers running concurrently must no longer treat the
    // device as being in provisioning mode while the SoftAP is torn down.
    s_wifi_config_mode = false;
    wifi_config_stop_softap();

    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    // Clear configured WiFi info when stopping
    s_configured_ssid[0] = '\0';
    s_configured_password[0] = '\0';

    ESP_LOGI(WIFI_CONFIG_TAG, "WiFi configuration mode stopped");
    return ESP_OK;
}

esp_err_t esp_br_wifi_config_get_configured_wifi(char *ssid, size_t ssid_len, char *password, size_t password_len,
                                                 uint32_t timeout_ms)
{
    if (!s_wifi_config_mode) {
        return ESP_ERR_INVALID_STATE;
    }

    assert(s_wifi_event_group);

    // If not yet configured, wait for configuration event
    if (s_configured_ssid[0] == '\0') {
        // Clear configured bit before waiting
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONFIGURED_BIT);

        // Wait for configuration event
        TickType_t timeout_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONFIGURED_BIT, pdTRUE, pdFALSE, timeout_ticks);

        if (!(bits & WIFI_CONFIGURED_BIT)) {
            return ESP_ERR_TIMEOUT;
        }
    }

    // Copy configured WiFi credentials to output buffers
    if (ssid && ssid_len > 0) {
        strncpy(ssid, s_configured_ssid, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
    }

    if (password && password_len > 0) {
        strncpy(password, s_configured_password, password_len - 1);
        password[password_len - 1] = '\0';
    }

    return ESP_OK;
}

bool esp_br_wifi_config_is_active(void)
{
    return s_wifi_config_mode;
}

esp_err_t esp_br_wifi_config_get_softap_info(char *ssid, size_t ssid_len, char *ip_addr, size_t ip_addr_len)
{
    if (!s_wifi_config_mode) {
        return ESP_ERR_INVALID_STATE;
    }

    if (ssid && ssid_len) {
        strncpy(ssid, s_softap_ssid, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
    }

    if (ip_addr && ip_addr_len > 0 && s_ap_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
            snprintf(ip_addr, ip_addr_len, IPSTR, IP2STR(&ip_info.ip));
        } else {
            // Fallback to default IP
            strncpy(ip_addr, "192.168.4.1", ip_addr_len - 1);
            ip_addr[ip_addr_len - 1] = '\0';
        }
    }

    return ESP_OK;
}
