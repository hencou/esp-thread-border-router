set(requires mdns fatfs spiffs esp_eth nvs_flash freertos openthread esp_http_server protocol_examples_common esp_wifi esp_app_format esp_timer console
             app_update esp_http_client esp_https_ota mbedtls)

# For IDF < 6.0, use IDF's built-in json component; for IDF >= 6.0 use managed cJSON component through idf_component.yml
if("${IDF_VERSION_MAJOR}" VERSION_LESS "6")
    list(APPEND requires json)
endif()

idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS private_include
    PRIV_REQUIRES ${requires}
    EMBED_FILES "favicon.ico"
)

# Wi-Fi credentials in NVS are managed by the CLI extension, combined host+RCP images by esp_br_http_ota
idf_component_optional_requires(PRIVATE espressif__esp_ot_cli_extension esp_ot_cli_extension)
if(CONFIG_OPENTHREAD_CLI_OTA)
    idf_component_optional_requires(PRIVATE esp_br_http_ota)
endif()

spiffs_create_partition_image(web_storage ${CMAKE_CURRENT_SOURCE_DIR}/frontend FLASH_IN_PROJECT)
