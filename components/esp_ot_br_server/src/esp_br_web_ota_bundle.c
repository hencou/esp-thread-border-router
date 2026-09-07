/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_br_web_ota_bundle.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"

#define BUNDLE_TAG "ota_bundle"
#define BUNDLE_MAGIC "OTBRBNDL"
#define BUNDLE_MAGIC_LEN 8
#define BUNDLE_VERSION 1
#define BUNDLE_MAX_ENTRIES 8
#define BUNDLE_TARGET_LEN 16
#define BUNDLE_CHUNK_SIZE 4096
#define BUNDLE_APP_TARGET "app"
#define BUNDLE_SHA256_LEN 32
#define FLASH_WRITE_ALIGNMENT 4

typedef struct {
    char magic[BUNDLE_MAGIC_LEN];
    uint32_t version;
    uint32_t entry_count;
} __attribute__((packed)) bundle_header_t;

typedef struct {
    char target[BUNDLE_TARGET_LEN];
    uint32_t size;
    uint8_t sha256[BUNDLE_SHA256_LEN];
} __attribute__((packed)) bundle_entry_t;

typedef struct {
    esp_br_ota_read_fn read;
    void *ctx;
    const char *prefetched;
    size_t prefetched_len;
} bundle_reader_t;

/** @brief Read exactly @p len bytes, or fail when the stream ends early. */
static esp_err_t reader_read_exact(bundle_reader_t *reader, void *buf, size_t len)
{
    char *out = buf;
    size_t done = 0;

    while (done < len) {
        if (reader->prefetched_len > 0) {
            size_t take = (reader->prefetched_len < len - done) ? reader->prefetched_len : len - done;
            memcpy(out + done, reader->prefetched, take);
            reader->prefetched += take;
            reader->prefetched_len -= take;
            done += take;
            continue;
        }
        int received = reader->read(reader->ctx, out + done, len - done);
        ESP_RETURN_ON_FALSE(received > 0, ESP_FAIL, BUNDLE_TAG, "Update image ended after %u of %u bytes",
                            (unsigned)done, (unsigned)len);
        done += received;
    }
    return ESP_OK;
}

/** @brief Destination of one bundle entry: either the inactive OTA slot or a data partition. */
typedef struct {
    esp_ota_handle_t app_handle;
    const esp_partition_t *partition;
    size_t offset;
} entry_writer_t;

static esp_err_t entry_writer_open(entry_writer_t *writer, const char *target, size_t size)
{
    memset(writer, 0, sizeof(*writer));

    if (strcmp(target, BUNDLE_APP_TARGET) == 0) {
        writer->partition = esp_ota_get_next_update_partition(NULL);
        ESP_RETURN_ON_FALSE(writer->partition != NULL, ESP_ERR_NOT_FOUND, BUNDLE_TAG, "No OTA partition available");
        ESP_RETURN_ON_FALSE(size <= writer->partition->size, ESP_ERR_INVALID_SIZE, BUNDLE_TAG,
                            "Application image of %u bytes does not fit in the %u-byte OTA partition", (unsigned)size,
                            (unsigned)writer->partition->size);
        return esp_ota_begin(writer->partition, size, &writer->app_handle);
    }

    writer->partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, target);
    ESP_RETURN_ON_FALSE(writer->partition != NULL, ESP_ERR_NOT_FOUND, BUNDLE_TAG, "No partition labelled '%s'", target);
    ESP_RETURN_ON_FALSE(size <= writer->partition->size, ESP_ERR_INVALID_SIZE, BUNDLE_TAG,
                        "Image of %u bytes does not fit in the %u-byte '%s' partition", (unsigned)size,
                        (unsigned)writer->partition->size, target);
    return esp_partition_erase_range(writer->partition, 0, writer->partition->size);
}

static esp_err_t entry_writer_write(entry_writer_t *writer, void *data, size_t len)
{
    if (writer->app_handle) {
        return esp_ota_write(writer->app_handle, data, len);
    }

    // Raw flash writes have to be aligned, so the final chunk of an image whose size is not a
    // multiple of the alignment is padded with erased-flash bytes.
    size_t aligned = (len + FLASH_WRITE_ALIGNMENT - 1) & ~(size_t)(FLASH_WRITE_ALIGNMENT - 1);
    memset((char *)data + len, 0xff, aligned - len);
    esp_err_t ret = esp_partition_write(writer->partition, writer->offset, data, aligned);
    writer->offset += aligned;
    return ret;
}

static esp_err_t entry_writer_close(entry_writer_t *writer)
{
    if (!writer->app_handle) {
        return ESP_OK;
    }
    esp_err_t ret = esp_ota_end(writer->app_handle);
    writer->app_handle = 0;
    return ret;
}

static void entry_writer_abort(entry_writer_t *writer)
{
    if (writer->app_handle) {
        esp_ota_abort(writer->app_handle);
        writer->app_handle = 0;
    }
}

/** @brief Stream @p size bytes into @p target and check them against @p expected_sha256. */
static esp_err_t apply_entry(bundle_reader_t *reader, const char *target, size_t size, const uint8_t *expected_sha256,
                             size_t *written)
{
    esp_err_t ret = ESP_OK;
    entry_writer_t writer = {0};
    mbedtls_sha256_context sha = {0};
    bool sha_started = false;
    // The chunk is padded in place when the image size is not a multiple of the flash alignment.
    char *chunk = malloc(BUNDLE_CHUNK_SIZE + FLASH_WRITE_ALIGNMENT);

    ESP_RETURN_ON_FALSE(chunk != NULL, ESP_ERR_NO_MEM, BUNDLE_TAG, "Failed to allocate the update buffer");
    ESP_GOTO_ON_ERROR(entry_writer_open(&writer, target, size), exit, BUNDLE_TAG, "Failed to prepare '%s'", target);

    if (expected_sha256) {
        mbedtls_sha256_init(&sha);
        ESP_GOTO_ON_FALSE(mbedtls_sha256_starts(&sha, 0) == 0, ESP_FAIL, exit, BUNDLE_TAG, "Failed to start SHA-256");
        sha_started = true;
    }

    ESP_LOGI(BUNDLE_TAG, "Writing %u bytes to '%s'", (unsigned)size, target);
    size_t remaining = size;
    while (remaining > 0) {
        size_t want = (remaining < BUNDLE_CHUNK_SIZE) ? remaining : BUNDLE_CHUNK_SIZE;
        ESP_GOTO_ON_ERROR(reader_read_exact(reader, chunk, want), exit, BUNDLE_TAG, "Failed to read '%s'", target);
        if (sha_started) {
            ESP_GOTO_ON_FALSE(mbedtls_sha256_update(&sha, (const unsigned char *)chunk, want) == 0, ESP_FAIL, exit,
                              BUNDLE_TAG, "Failed to hash '%s'", target);
        }
        ESP_GOTO_ON_ERROR(entry_writer_write(&writer, chunk, want), exit, BUNDLE_TAG, "Failed to write '%s'", target);
        remaining -= want;
        if (written) {
            *written += want;
        }
    }

    if (sha_started) {
        uint8_t digest[BUNDLE_SHA256_LEN];
        ESP_GOTO_ON_FALSE(mbedtls_sha256_finish(&sha, digest) == 0, ESP_FAIL, exit, BUNDLE_TAG,
                          "Failed to hash '%s'", target);
        ESP_GOTO_ON_FALSE(memcmp(digest, expected_sha256, sizeof(digest)) == 0, ESP_ERR_INVALID_CRC, exit, BUNDLE_TAG,
                          "Checksum mismatch for '%s'", target);
    }

    ESP_GOTO_ON_ERROR(entry_writer_close(&writer), exit, BUNDLE_TAG, "Failed to verify '%s'", target);

exit:
    if (sha_started) {
        mbedtls_sha256_free(&sha);
    }
    if (ret != ESP_OK) {
        entry_writer_abort(&writer);
    }
    free(chunk);
    return ret;
}

/** @brief Write an application image of unknown length until the stream ends. */
static esp_err_t apply_app_until_eof(bundle_reader_t *reader, size_t *written)
{
    esp_err_t ret = ESP_OK;
    esp_ota_handle_t handle = 0;
    char *chunk = malloc(BUNDLE_CHUNK_SIZE);
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);

    ESP_RETURN_ON_FALSE(chunk != NULL, ESP_ERR_NO_MEM, BUNDLE_TAG, "Failed to allocate the update buffer");
    ESP_GOTO_ON_FALSE(partition != NULL, ESP_ERR_NOT_FOUND, exit, BUNDLE_TAG, "No OTA partition available");
    ESP_GOTO_ON_ERROR(esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle), exit, BUNDLE_TAG,
                      "Failed to begin the application update");

    while (true) {
        size_t want = BUNDLE_CHUNK_SIZE;
        int received;

        if (reader->prefetched_len > 0) {
            received = (int)((reader->prefetched_len < want) ? reader->prefetched_len : want);
            memcpy(chunk, reader->prefetched, received);
            reader->prefetched += received;
            reader->prefetched_len -= received;
        } else {
            received = reader->read(reader->ctx, chunk, want);
            ESP_GOTO_ON_FALSE(received >= 0, ESP_FAIL, exit, BUNDLE_TAG, "Failed to read the application image");
            if (received == 0) {
                break;
            }
        }
        ESP_GOTO_ON_ERROR(esp_ota_write(handle, chunk, received), exit, BUNDLE_TAG,
                          "Failed to write the application image");
        if (written) {
            *written += received;
        }
    }

    ret = esp_ota_end(handle);
    handle = 0;
    ESP_GOTO_ON_ERROR(ret, exit, BUNDLE_TAG, "Failed to verify the application image");

exit:
    if (ret != ESP_OK && handle) {
        esp_ota_abort(handle);
    }
    free(chunk);
    return ret;
}

static esp_err_t apply_bundle(bundle_reader_t *reader, const bundle_header_t *header, size_t *written, size_t *total)
{
    esp_err_t ret = ESP_OK;
    bundle_entry_t *entries = NULL;
    bool app_updated = false;

    ESP_RETURN_ON_FALSE(header->version == BUNDLE_VERSION, ESP_ERR_INVALID_VERSION, BUNDLE_TAG,
                        "Unsupported bundle version %u", (unsigned)header->version);
    ESP_RETURN_ON_FALSE(header->entry_count > 0 && header->entry_count <= BUNDLE_MAX_ENTRIES, ESP_ERR_INVALID_SIZE,
                        BUNDLE_TAG, "Bundle declares %u entries", (unsigned)header->entry_count);

    entries = calloc(header->entry_count, sizeof(bundle_entry_t));
    ESP_RETURN_ON_FALSE(entries != NULL, ESP_ERR_NO_MEM, BUNDLE_TAG, "Failed to allocate the bundle index");
    ESP_GOTO_ON_ERROR(reader_read_exact(reader, entries, header->entry_count * sizeof(bundle_entry_t)), exit,
                      BUNDLE_TAG, "Failed to read the bundle index");

    if (total) {
        *total = 0;
        for (uint32_t i = 0; i < header->entry_count; i++) {
            *total += entries[i].size;
        }
    }

    for (uint32_t i = 0; i < header->entry_count; i++) {
        bundle_entry_t *entry = &entries[i];
        ESP_GOTO_ON_FALSE(entry->target[BUNDLE_TARGET_LEN - 1] == '\0', ESP_ERR_INVALID_ARG, exit, BUNDLE_TAG,
                          "Bundle entry %u has an invalid target", (unsigned)i);
        ESP_GOTO_ON_ERROR(apply_entry(reader, entry->target, entry->size, entry->sha256, written), exit, BUNDLE_TAG,
                          "Failed to apply the bundle");
        if (strcmp(entry->target, BUNDLE_APP_TARGET) == 0) {
            app_updated = true;
        }
    }

    // The boot partition is switched only once every partition has been written, so a failure
    // halfway through leaves the device running the current firmware.
    if (app_updated) {
        ESP_GOTO_ON_ERROR(esp_ota_set_boot_partition(esp_ota_get_next_update_partition(NULL)), exit, BUNDLE_TAG,
                          "Failed to set the boot partition");
    }

exit:
    free(entries);
    return ret;
}

size_t esp_br_ota_detect_len(void)
{
    return sizeof(bundle_header_t);
}

esp_err_t esp_br_ota_apply_stream(const void *prefetched, size_t prefetched_len, esp_br_ota_read_fn read_fn, void *ctx,
                                  size_t total_size, size_t *written, size_t *total)
{
    bundle_reader_t reader = {
        .read = read_fn,
        .ctx = ctx,
        .prefetched = prefetched,
        .prefetched_len = prefetched_len,
    };
    bundle_header_t header;

    ESP_RETURN_ON_FALSE(read_fn != NULL, ESP_ERR_INVALID_ARG, BUNDLE_TAG, "No reader given");
    ESP_RETURN_ON_ERROR(reader_read_exact(&reader, &header, sizeof(header)), BUNDLE_TAG,
                        "Failed to read the image header");

    if (memcmp(header.magic, BUNDLE_MAGIC, BUNDLE_MAGIC_LEN) == 0) {
        return apply_bundle(&reader, &header, written, total);
    }

    // A plain application image: the header bytes are part of it, so they are pushed back in front
    // of the stream.
    ESP_RETURN_ON_FALSE(reader.prefetched_len == 0, ESP_ERR_INVALID_ARG, BUNDLE_TAG,
                        "More bytes were prefetched than the header needs");
    bundle_reader_t app_reader = {
        .read = read_fn,
        .ctx = ctx,
        .prefetched = (const char *)&header,
        .prefetched_len = sizeof(header),
    };

    if (total_size > 0) {
        if (total) {
            *total = total_size;
        }
        ESP_RETURN_ON_ERROR(apply_entry(&app_reader, BUNDLE_APP_TARGET, total_size, NULL, written), BUNDLE_TAG,
                            "Failed to write the application image");
    } else {
        ESP_RETURN_ON_ERROR(apply_app_until_eof(&app_reader, written), BUNDLE_TAG,
                            "Failed to write the application image");
    }
    return esp_ota_set_boot_partition(esp_ota_get_next_update_partition(NULL));
}
