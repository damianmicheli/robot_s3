#include "network.h"

#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "private_config.h"

#define CONNECTED_BIT BIT0
#define FAILED_BIT BIT1
static EventGroupHandle_t s_events;
static bool s_initialized;
static uint8_t s_retries;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retries++ < 5) esp_wifi_connect();
        else xEventGroupSetBits(s_events, FAILED_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retries = 0;
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

esp_err_t network_connect(void)
{
    if (!s_initialized) {
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_flash_erase(), "WIFI", "NVS");
            ESP_RETURN_ON_ERROR(nvs_flash_init(), "WIFI", "NVS");
        } else ESP_RETURN_ON_ERROR(err, "WIFI", "NVS");
        ESP_RETURN_ON_ERROR(esp_netif_init(), "WIFI", "netif");
        ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), "WIFI", "event loop");
        s_events = xEventGroupCreate();
        if (!s_events) return ESP_ERR_NO_MEM;
        esp_netif_create_default_wifi_sta();
        wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_wifi_init(&init), "WIFI", "init");
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL), "WIFI", "handler");
        ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL), "WIFI", "handler");
        wifi_config_t config = {0};
        strlcpy((char *)config.sta.ssid, WIFI_SSID, sizeof(config.sta.ssid));
        strlcpy((char *)config.sta.password, WIFI_PASSWORD, sizeof(config.sta.password));
        config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), "WIFI", "mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), "WIFI", "config");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), "WIFI", "start");
        s_initialized = true;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT | FAILED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    return (bits & CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}
