#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "freertos/queue.h"

// One CSI measurement and its metadata
typedef struct {
    uint32_t timestamp;
    uint16_t len;
    int8_t rssi;
    bool first_word_invalid;
    int8_t data[128];
} csi_sample_t;

static QueueHandle_t csi_queue;

// The Wi-Fi driver calls this when CSI arrives
static void on_csi(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf ||
        info->len > sizeof(((csi_sample_t *)0)->data)) {
        return;
    }

    csi_sample_t sample = {
        .timestamp = info->rx_ctrl.timestamp,
        .len = info->len,
        .rssi = info->rx_ctrl.rssi,
        .first_word_invalid = info->first_word_invalid,
    };

    memcpy(sample.data, info->buf, info->len);

    // Copy into the queue without waiting
    xQueueSend(csi_queue, &sample, 0);
}

static void on_got_ip(void *arg, esp_event_base_t base,
                      int32_t id, void *data)
{
    ip_event_got_ip_t *event = data;

    printf("ESP32 IP: " IPSTR "\n",
           IP2STR(&event->ip_info.ip));

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // Start with CSI from the legacy long training field
    wifi_csi_config_t csi_config = {
        .lltf_en = true,
        .htltf_en = false,
        .stbc_htltf2_en = false,
        .ltf_merge_en = false,
        .channel_filter_en = false,
        .manu_scale = false,
    };

    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(on_csi, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));

    printf("CSI enabled\n");
}
static void on_wifi_disconnected(void *arg, esp_event_base_t base,
                                 int32_t id, void *data)
{
    wifi_event_sta_disconnected_t *event = data;

    printf("Wi-Fi disconnected: reason=%u\n",
           (unsigned)event->reason);
}
void app_main(void)
{
    csi_queue = xQueueCreate(8, sizeof(csi_sample_t));
    configASSERT(csi_queue != NULL);
    // Prepare storage used by the Wi-Fi driver
    ESP_ERROR_CHECK(nvs_flash_init());

    // Prepare networking and event handling
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // Initialize the Wi-Fi driver with default settings
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));

    printf("Wi-Fi initialized\n");
    wifi_config_t router_config = {
        .sta = {
            .ssid = "paleisje01",
            .password = "imerseal",
        },
    };

    // Register our function for the "received IP address" event
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        on_got_ip,
        NULL
    ));

    // Configure the board as a Wi-Fi client
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &router_config));

    printf("Connecting to Wi-Fi...\n");
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT,
        WIFI_EVENT_STA_DISCONNECTED,
        on_wifi_disconnected,
        NULL
    ));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_connect());
    printf("Connection attempt requested\n");
    csi_sample_t sample;

    while (1)
    {
        // Wait until a measurement becomes available
        if (xQueueReceive(csi_queue, &sample, portMAX_DELAY) == pdTRUE)
        {
            printf("CSI,%lu,%d,%u,%u,\"[",
                (unsigned long)sample.timestamp,
                (int)sample.rssi,
                (unsigned)sample.len,
                (unsigned)sample.first_word_invalid);

            for (int i = 0; i < sample.len; i++)
            {
                printf("%s%d", i == 0 ? "" : ",",
                    (int)sample.data[i]);
            }

            printf("]\"\n");
        }
    }
}
