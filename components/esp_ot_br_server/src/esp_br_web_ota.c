/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_br_web_ota.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#if CONFIG_OPENTHREAD_CLI_OTA
#include "esp_br_http_ota.h"
#endif

#define OTA_TAG "web_ota"
#define OTA_RECV_BUFFER_SIZE 2048
#define OTA_URL_MAX_LEN 512
#define OTA_MESSAGE_MAX_LEN 96
#define OTA_REBOOT_DELAY_MS 1500

typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_RUNNING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED,
} ota_state_t;

typedef struct {
    char url[OTA_URL_MAX_LEN];
    bool combined;
} ota_url_request_t;

static SemaphoreHandle_t s_ota_lock = NULL;
static ota_state_t s_ota_state = OTA_STATE_IDLE;
static size_t s_ota_written = 0;
static size_t s_ota_total = 0;
static char s_ota_message[OTA_MESSAGE_MAX_LEN] = "";

static const char *ota_state_to_string(ota_state_t state)
{
    switch (state) {
    case OTA_STATE_RUNNING:
        return "running";
    case OTA_STATE_SUCCESS:
        return "success";
    case OTA_STATE_FAILED:
        return "failed";
    default:
        return "idle";
    }
}

/** @brief Claim the exclusive right to write to the inactive OTA partition. */
static bool ota_try_acquire(void)
{
    static portMUX_TYPE lock_init_mux = portMUX_INITIALIZER_UNLOCKED;

    portENTER_CRITICAL(&lock_init_mux);
    if (s_ota_lock == NULL) {
        s_ota_lock = xSemaphoreCreateMutex();
    }
    portEXIT_CRITICAL(&lock_init_mux);
    if (s_ota_lock == NULL) {
        return false;
    }

    if (xSemaphoreTake(s_ota_lock, 0) != pdTRUE) {
        return false;
    }
    if (s_ota_state == OTA_STATE_RUNNING) {
        xSemaphoreGive(s_ota_lock);
        return false;
    }

    s_ota_state = OTA_STATE_RUNNING;
    s_ota_written = 0;
    s_ota_total = 0;
    s_ota_message[0] = '\0';
    xSemaphoreGive(s_ota_lock);
    return true;
}

static void ota_finish(esp_err_t err, const char *message)
{
    s_ota_state = (err == ESP_OK) ? OTA_STATE_SUCCESS : OTA_STATE_FAILED;
    strlcpy(s_ota_message, message, sizeof(s_ota_message));
    if (err == ESP_OK) {
        ESP_LOGI(OTA_TAG, "%s", message);
    } else {
        ESP_LOGE(OTA_TAG, "%s: %s", message, esp_err_to_name(err));
    }
}

static void ota_reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    ESP_LOGW(OTA_TAG, "Restarting into the new firmware");
    esp_restart();
}

static void ota_schedule_reboot(void)
{
    if (xTaskCreate(ota_reboot_task, "ota_reboot", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(OTA_TAG, "Failed to create reboot task");
    }
}

static esp_err_t ota_send_conflict(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_send(req, "{\"success\":false,\"message\":\"Another update is already running\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t ota_send_json_result(httpd_req_t *req, esp_err_t err, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_NO_MEM, OTA_TAG, "Failed to allocate JSON object");

    cJSON_AddBoolToObject(root, "success", err == ESP_OK);
    cJSON_AddStringToObject(root, "message", message);
    if (err != ESP_OK) {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(err));
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(json_str != NULL, ESP_ERR_NO_MEM, OTA_TAG, "Failed to print JSON object");

    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    httpd_resp_send(req, json_str, strlen(json_str));
    cJSON_free(json_str);
    return ESP_OK;
}

esp_err_t esp_br_web_ota_status_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate memory");
        return ESP_ERR_NO_MEM;
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    cJSON_AddStringToObject(root, "state", ota_state_to_string(s_ota_state));
    cJSON_AddStringToObject(root, "message", s_ota_message);
    cJSON_AddNumberToObject(root, "written", s_ota_written);
    cJSON_AddNumberToObject(root, "total", s_ota_total);
    cJSON_AddNumberToObject(root, "progress", s_ota_total > 0 ? (100 * s_ota_written / s_ota_total) : 0);
    cJSON_AddStringToObject(root, "project_name", app_desc->project_name);
    cJSON_AddStringToObject(root, "version", app_desc->version);
    cJSON_AddStringToObject(root, "idf_version", app_desc->idf_ver);
    cJSON_AddStringToObject(root, "compile_time", app_desc->date);
    cJSON_AddStringToObject(root, "running_partition", running ? running->label : "");

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    cJSON_AddStringToObject(root, "update_partition", update ? update->label : "");
    cJSON_AddNumberToObject(root, "update_partition_size", update ? update->size : 0);
#if CONFIG_OPENTHREAD_CLI_OTA
    cJSON_AddBoolToObject(root, "combined_supported", true);
#else
    cJSON_AddBoolToObject(root, "combined_supported", false);
#endif

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

esp_err_t esp_br_web_ota_upload_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty firmware image");
        return ESP_ERR_INVALID_ARG;
    }

    if (!ota_try_acquire()) {
        return ota_send_conflict(req);
    }

    esp_err_t ret = ESP_OK;
    esp_ota_handle_t ota_handle = 0;
    char *buffer = NULL;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    ESP_GOTO_ON_FALSE(update_partition != NULL, ESP_ERR_NOT_FOUND, exit, OTA_TAG, "No OTA partition available");
    ESP_GOTO_ON_FALSE(req->content_len <= update_partition->size, ESP_ERR_INVALID_SIZE, exit, OTA_TAG,
                      "Firmware image of %u bytes does not fit in the %u-byte OTA partition",
                      (unsigned)req->content_len, (unsigned)update_partition->size);

    buffer = malloc(OTA_RECV_BUFFER_SIZE);
    ESP_GOTO_ON_FALSE(buffer != NULL, ESP_ERR_NO_MEM, exit, OTA_TAG, "Failed to allocate receive buffer");

    s_ota_total = req->content_len;
    ESP_GOTO_ON_ERROR(esp_ota_begin(update_partition, req->content_len, &ota_handle), exit, OTA_TAG,
                      "Failed to begin OTA");

    while (s_ota_written < s_ota_total) {
        int received = httpd_req_recv(req, buffer, OTA_RECV_BUFFER_SIZE);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        ESP_GOTO_ON_FALSE(received > 0, ESP_FAIL, exit, OTA_TAG, "Failed to receive firmware image");
        ESP_GOTO_ON_ERROR(esp_ota_write(ota_handle, buffer, received), exit, OTA_TAG, "Failed to write firmware image");
        s_ota_written += received;
    }

    ret = esp_ota_end(ota_handle);
    ota_handle = 0;
    ESP_GOTO_ON_ERROR(ret, exit, OTA_TAG, "Failed to verify firmware image");
    ESP_GOTO_ON_ERROR(esp_ota_set_boot_partition(update_partition), exit, OTA_TAG, "Failed to set boot partition");

exit:
    free(buffer);
    if (ret != ESP_OK && ota_handle) {
        esp_ota_abort(ota_handle);
    }
    ota_finish(ret, ret == ESP_OK ? "Firmware uploaded, restarting" : "Firmware upload failed");
    ota_send_json_result(req, ret, s_ota_message);
    if (ret == ESP_OK) {
        ota_schedule_reboot();
    }
    return ret;
}

static void ota_url_task(void *arg)
{
    ota_url_request_t *request = (ota_url_request_t *)arg;
    esp_err_t ret = ESP_OK;

    esp_http_client_config_t http_config = {
        .url = request->url,
        .timeout_ms = 30 * 1000,
        .keep_alive_enable = true,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };

    if (request->combined) {
#if CONFIG_OPENTHREAD_CLI_OTA
        // A combined image also carries the RCP firmware, which esp_br_http_ota() unpacks and
        // stages for the RCP update that runs on the next boot.
        ret = esp_br_http_ota(&http_config);
#else
        ret = ESP_ERR_NOT_SUPPORTED;
#endif
    } else {
        esp_https_ota_config_t ota_config = {
            .http_config = &http_config,
        };
        esp_https_ota_handle_t ota_handle = NULL;
        ret = esp_https_ota_begin(&ota_config, &ota_handle);
        if (ret == ESP_OK) {
            s_ota_total = esp_https_ota_get_image_size(ota_handle);
            do {
                ret = esp_https_ota_perform(ota_handle);
                s_ota_written = esp_https_ota_get_image_len_read(ota_handle);
            } while (ret == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

            if (ret == ESP_OK && !esp_https_ota_is_complete_data_received(ota_handle)) {
                ret = ESP_FAIL;
            }
            esp_err_t finish_err = esp_https_ota_finish(ota_handle);
            if (ret == ESP_OK) {
                ret = finish_err;
            }
        }
    }

    ota_finish(ret, ret == ESP_OK ? "Firmware downloaded, restarting" : "Firmware download failed");
    free(request);
    if (ret == ESP_OK) {
        ota_schedule_reboot();
    }
    vTaskDelete(NULL);
}

esp_err_t esp_br_web_ota_url_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > OTA_URL_MAX_LEN + 64) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_ERR_INVALID_ARG;
    }

    char body[OTA_URL_MAX_LEN + 65];
    int received = httpd_req_recv(req, body, req->content_len);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive request body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    cJSON *json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const cJSON *url = cJSON_GetObjectItem(json, "url");
    const cJSON *combined = cJSON_GetObjectItem(json, "combined");
    if (!cJSON_IsString(url) || strlen(url->valuestring) == 0 || strlen(url->valuestring) >= OTA_URL_MAX_LEN) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing or invalid URL");
        return ESP_ERR_INVALID_ARG;
    }

    ota_url_request_t *request = calloc(1, sizeof(ota_url_request_t));
    if (!request) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate memory");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(request->url, url->valuestring, sizeof(request->url));
    request->combined = cJSON_IsTrue(combined);
    cJSON_Delete(json);

#if !CONFIG_OPENTHREAD_CLI_OTA
    if (request->combined) {
        free(request);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Combined images are not supported by this firmware");
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif

    if (!ota_try_acquire()) {
        free(request);
        return ota_send_conflict(req);
    }

    // The download runs in its own task so that the HTTP server stays responsive and the web GUI
    // can poll the progress.
    if (xTaskCreate(ota_url_task, "ota_url", 8192, request, 5, NULL) != pdPASS) {
        ota_finish(ESP_ERR_NO_MEM, "Failed to start the download task");
        free(request);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to start the download task");
        return ESP_ERR_NO_MEM;
    }

    return ota_send_json_result(req, ESP_OK, "Firmware download started");
}
