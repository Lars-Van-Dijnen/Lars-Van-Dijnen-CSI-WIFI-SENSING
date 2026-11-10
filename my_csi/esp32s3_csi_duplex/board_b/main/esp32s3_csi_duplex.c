/*
 * ESP32-S3 bidirectional CSI demo -- ESP-IDF 6.1.
 *
 * Both boards receive CSI. A sends REQUEST; B records CSI and sends REPLY;
 * A records CSI and starts the next cycle. No router, IP address or ping needed.
 *
 * Use the bundled Board A / Board B projects. Alternatively, define CSI_NODE_ID
 * as 1 or 2 before the configuration block below when using this source alone.
 * All radio callbacks only copy data into queues. Worker tasks do the rest.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 1. Small, explicitly encoded protocol. No struct padding goes over the air. */
enum { PROBE_SIZE = 16, PROBE_REQUEST = 1, PROBE_REPLY = 2 };

typedef struct {
    uint8_t sender;
    uint8_t kind;
    uint32_t session;
    uint32_t sequence;
} probe_t;

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_u32_le(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) {
        p[i] = (uint8_t)(value >> (8 * i));
    }
}

static void encode_probe(uint8_t out[PROBE_SIZE], const probe_t *probe)
{
    memcpy(out, "S3CS", 4);
    out[4] = 1;                  /* Our protocol version. */
    out[5] = probe->sender;
    out[6] = probe->kind;
    out[7] = 0;
    write_u32_le(out + 8, probe->session);
    write_u32_le(out + 12, probe->sequence);
}

static bool decode_probe(const uint8_t *data, size_t len,
                         uint8_t expected_sender, probe_t *out)
{
    if (!data || !out || len != PROBE_SIZE ||
        memcmp(data, "S3CS", 4) != 0 || data[4] != 1 || data[7] != 0 ||
        data[5] != expected_sender) {
        return false;
    }
    if (!((data[5] == 1 && data[6] == PROBE_REQUEST) ||
          (data[5] == 2 && data[6] == PROBE_REPLY))) {
        return false;
    }
    *out = (probe_t) {
        .sender = data[5], .kind = data[6],
        .session = read_u32_le(data + 8),
        .sequence = read_u32_le(data + 12),
    };
    return true;
}

/* CSI supplies the 802.11 action-frame body, not just ESP-NOW application data.
 * For our short, unencrypted packets there is one vendor element. Its body
 * starts after 15 bytes: category + OUI + random + vendor-element header.
 * This is also the layout used by Espressif's CSI examples. Validate lengths
 * before reading; never invent a cycle number if the layout is unavailable.
 */
static bool decode_csi_probe(const uint8_t *body, size_t len,
                             uint8_t expected_sender, probe_t *out)
{
    static const uint8_t oui[3] = {0x18, 0xfe, 0x34};
    if (!body || len < 15 + PROBE_SIZE || body[0] != 127 ||
        memcmp(body + 1, oui, 3) != 0 || body[8] != 221 ||
        memcmp(body + 10, oui, 3) != 0 || body[13] != 4) {
        return false;
    }
    const size_t element_len = body[9];
    const unsigned version = body[14] & 0x0f;
    if (element_len != 5 + PROBE_SIZE || element_len > len - 10 ||
        (version != 1 && version != 2) || (body[14] & 0x10) != 0) {
        return false;
    }
    return decode_probe(body + 15, PROBE_SIZE, expected_sender, out);
}

#ifndef CSI_HOST_TEST

#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "This project targets ESP32-S3. Run: idf.py set-target esp32s3"
#endif
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 1, 0)
#error "Use ESP-IDF 6.1 or newer for this source's ESP-NOW callback API."
#endif
#if !defined(CONFIG_ESP_WIFI_CSI_ENABLED) || !CONFIG_ESP_WIFI_CSI_ENABLED
#error "Enable Component config > Wi-Fi > WiFi CSI before building."
#endif

/* 2. Settings. The supplied projects select A/B through sdkconfig.defaults. */
#ifndef CSI_NODE_ID
#if defined(CONFIG_CSI_BOARD_B) && CONFIG_CSI_BOARD_B
#define CSI_NODE_ID 2
#else
#define CSI_NODE_ID 1
#endif
#endif
#if CSI_NODE_ID != 1 && CSI_NODE_ID != 2
#error "CSI_NODE_ID must be 1 (A) or 2 (B)."
#endif
#define CSI_PEER_ID (3 - CSI_NODE_ID)

#ifndef CONFIG_CSI_CHANNEL
#define CONFIG_CSI_CHANNEL 6
#endif
#ifndef CONFIG_CSI_CYCLE_MS
#define CONFIG_CSI_CYCLE_MS 100
#endif
#ifndef CONFIG_CSI_REPLY_WAIT_MS
#define CONFIG_CSI_REPLY_WAIT_MS 80
#endif
#ifndef CONFIG_CSI_REPLY_DELAY_MS
#define CONFIG_CSI_REPLY_DELAY_MS 5
#endif

enum { CSI_MAX_BYTES = 128, CSI_QUEUE_DEPTH = 24, RX_QUEUE_DEPTH = 8 };
static const char *TAG = "csi_duplex";

/* Locally administered demo MAC addresses. No manual MAC discovery is needed.
 * Use a different prefix if running multiple independent pairs together.
 */
static const uint8_t local_mac[6] = {0x02, 0x43, 0x53, 0x49, 0x00, CSI_NODE_ID};
static const uint8_t peer_mac[6] = {0x02, 0x43, 0x53, 0x49, 0x00, CSI_PEER_ID};

typedef struct {
    probe_t probe;
    bool tagged;
    uint32_t radio_time_us;       /* Driver clock; wraps after about 71.6 min. */
    int64_t uptime_us;            /* 64-bit local monotonic clock. */
    int8_t rssi;
    uint8_t channel;
    uint16_t len;
    bool first_word_invalid;
    int8_t data[CSI_MAX_BYTES];
} csi_record_t;

typedef struct {
    uint32_t tx_ok, tx_fail, send_errors, messages_rx, rx_queue_drops;
    uint32_t csi_rx, csi_queue_drops, csi_oversize, csi_unlabelled;
    uint32_t reply_timeouts, cycles;
} counters_t;

static QueueHandle_t csi_queue;
static QueueHandle_t message_queue;
static QueueHandle_t tx_done_queue;
static counters_t counters;
static portMUX_TYPE counter_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t boot_id;
static uint32_t session_id;

static void count_one(uint32_t *counter)
{
    portENTER_CRITICAL(&counter_lock);
    ++*counter;
    portEXIT_CRITICAL(&counter_lock);
}

static counters_t snapshot_counters(void)
{
    portENTER_CRITICAL(&counter_lock);
    counters_t result = counters;
    portEXIT_CRITICAL(&counter_lock);
    return result;
}

static TickType_t ticks_for_ms(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks ? ticks : 1;
}

/* 3. Wi-Fi callbacks: only validate/copy data. No serial output or waits here. */
static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    count_one(status == ESP_NOW_SEND_SUCCESS ? &counters.tx_ok : &counters.tx_fail);
    /* One send is outstanding at a time, so a one-item mailbox is sufficient. */
    xQueueOverwrite(tx_done_queue, &status);
}

static void on_message(const esp_now_recv_info_t *info,
                       const uint8_t *data, int len)
{
    probe_t probe;
    if (!info || !info->src_addr || len < 0 ||
        memcmp(info->src_addr, peer_mac, 6) != 0 ||
        !decode_probe(data, (size_t)len, CSI_PEER_ID, &probe)) {
        return;
    }
    count_one(&counters.messages_rx);
    if (xQueueSend(message_queue, &probe, 0) != pdTRUE) {
        count_one(&counters.rx_queue_drops);
    }
}

static void on_csi(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;
    if (!info || !info->buf || info->len == 0 ||
        memcmp(info->mac, peer_mac, 6) != 0 ||
        memcmp(info->dmac, local_mac, 6) != 0) {
        return;
    }
    if (info->len > CSI_MAX_BYTES) {
        count_one(&counters.csi_oversize);
        return;
    }
    csi_record_t record = {
        .radio_time_us = info->rx_ctrl.timestamp,
        .uptime_us = esp_timer_get_time(),
        .rssi = info->rx_ctrl.rssi,
        .channel = info->rx_ctrl.channel,
        .len = info->len,
        .first_word_invalid = info->first_word_invalid,
    };
    record.tagged = decode_csi_probe(info->payload, info->payload_len,
                                     CSI_PEER_ID, &record.probe);
    if (!record.tagged) {
        count_one(&counters.csi_unlabelled);
    }
    /* The driver owns info and its pointers; retain our own byte copy. */
    memcpy(record.data, info->buf, info->len);
    count_one(&counters.csi_rx);
    if (xQueueSend(csi_queue, &record, 0) != pdTRUE) {
        count_one(&counters.csi_queue_drops);
    }
}

/* 4. Transmit one probe, then wait for its send callback before any next send. */
static bool send_probe(uint8_t kind, uint32_t session, uint32_t sequence)
{
    probe_t probe = {
        .sender = CSI_NODE_ID, .kind = kind,
        .session = session, .sequence = sequence,
    };
    uint8_t wire[PROBE_SIZE];
    encode_probe(wire, &probe);
    esp_now_send_status_t status;
    /* The previous call consumed its completion. Drop any stale mailbox item. */
    while (xQueueReceive(tx_done_queue, &status, 0) == pdTRUE) { }

    esp_err_t err = esp_now_send(peer_mac, wire, sizeof(wire));
    if (err != ESP_OK) {
        count_one(&counters.send_errors);
        ESP_LOGW(TAG, "esp_now_send: %s", esp_err_to_name(err));
        return false;
    }
    if (xQueueReceive(tx_done_queue, &status, ticks_for_ms(1000)) != pdTRUE) {
        /* Sending again without a completion would misassociate callbacks. */
        ESP_LOGE(TAG, "Missing ESP-NOW send callback; restarting radio demo.");
        esp_restart();
        return false;
    }
    return status == ESP_NOW_SEND_SUCCESS;
}

static void exchange_task(void *arg)
{
    (void)arg;
    uint32_t sequence = 0;
    bool have_previous = false;
    probe_t previous = {0};

    for (;;) {
        if (CSI_NODE_ID == 1) {
            /* A starts every request/reply cycle. A never replies to a reply. */
            const int64_t started = esp_timer_get_time();
            const uint32_t current = sequence++;
            count_one(&counters.cycles);
            if (send_probe(PROBE_REQUEST, session_id, current)) {
                const int64_t deadline = esp_timer_get_time() +
                                        (int64_t)CONFIG_CSI_REPLY_WAIT_MS * 1000;
                bool got_reply = false;
                while (esp_timer_get_time() < deadline) {
                    probe_t reply;
                    int64_t left_us = deadline - esp_timer_get_time();
                    if (left_us <= 0) { break; }
                    if (xQueueReceive(message_queue, &reply,
                                      ticks_for_ms((uint32_t)((left_us + 999) / 1000)))
                        != pdTRUE) {
                        break;
                    }
                    if (reply.kind == PROBE_REPLY && reply.session == session_id &&
                        reply.sequence == current) {
                        got_reply = true;
                        break;
                    }
                }
                if (!got_reply) { count_one(&counters.reply_timeouts); }
            }
            const int64_t left_us = (int64_t)CONFIG_CSI_CYCLE_MS * 1000 -
                                    (esp_timer_get_time() - started);
            /* An overrun starts a later cycle, never a burst of catch-up sends. */
            vTaskDelay(left_us > 0
                       ? ticks_for_ms((uint32_t)((left_us + 999) / 1000)) : 1);
        } else {
            /* B can be powered first or later. A keeps trying if B is absent. */
            probe_t request;
            if (xQueueReceive(message_queue, &request, portMAX_DELAY) != pdTRUE) {
                continue;
            }
            if (request.kind != PROBE_REQUEST) { continue; }
            if (have_previous && previous.session == request.session &&
                previous.sequence == request.sequence) {
                continue;
            }
            previous = request;
            have_previous = true;
            vTaskDelay(ticks_for_ms(CONFIG_CSI_REPLY_DELAY_MS));
            send_probe(PROBE_REPLY, request.session, request.sequence);
        }
    }
}

/* 5. Print complete CSV lines in a lower-priority task, never a Wi-Fi callback. */
static void csi_output_task(void *arg)
{
    (void)arg;
    csi_record_t record;
    char line[1024];
    for (;;) {
        if (xQueueReceive(csi_queue, &record, portMAX_DELAY) != pdTRUE) { continue; }
        char session[16] = "";
        char sequence[16] = "";
        const char *kind = "OTHER";
        if (record.tagged) {
            snprintf(session, sizeof(session), "0x%08" PRIx32, record.probe.session);
            snprintf(sequence, sizeof(sequence), "%" PRIu32, record.probe.sequence);
            kind = record.probe.kind == PROBE_REQUEST ? "REQUEST" : "REPLY";
        }
        int n = snprintf(line, sizeof(line),
                         "CSI,%c,%c,%s,%s,%s,0x%08" PRIx32 ",%" PRIu32
                         ",%" PRId64 ",%d,%u,%u,%u,\"[",
                         'A' + CSI_NODE_ID - 1, 'A' + CSI_PEER_ID - 1,
                         kind, session, sequence, boot_id, record.radio_time_us,
                         record.uptime_us, (int)record.rssi, (unsigned)record.channel,
                         (unsigned)record.len, (unsigned)record.first_word_invalid);
        if (n < 0 || (size_t)n >= sizeof(line)) { continue; }
        size_t used = (size_t)n;
        bool overflow = false;
        for (unsigned i = 0; i < record.len; ++i) {
            n = snprintf(line + used, sizeof(line) - used,
                         "%s%d", i ? "," : "", (int)record.data[i]);
            if (n < 0 || (size_t)n >= sizeof(line) - used) {
                overflow = true;
                break;
            }
            used += (size_t)n;
        }
        if (overflow || used + 4 >= sizeof(line)) { continue; }
        memcpy(line + used, "]\"\n", 4);
        fputs(line, stdout);
    }
}

/* 6. Start a fixed-channel direct link with an OFDM rate that can produce CSI.
 * ESP-NOW's default 1-Mbit/s DSSS rate is intentionally replaced by HT20 MCS0.
 */
static void start_radio(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, local_mac));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_CSI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, peer_mac, sizeof(peer.peer_addr));
    peer.channel = CONFIG_CSI_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;         /* Only public demo IDs/counters are sent. */
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    esp_now_rate_config_t rate = {
        .phymode = WIFI_PHY_MODE_HT20,
        .rate = WIFI_PHY_RATE_MCS0_LGI,
        .ersu = false, .dcm = false,
    };
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(peer_mac, &rate));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_message));

    /* Match the LLTF-only, 128-byte format of your working router receiver.
     * Promiscuous mode makes CSI available without an AP association; on_csi
     * filters both MAC addresses so only the paired board's traffic is logged.
     */
    wifi_csi_config_t csi = {
        .lltf_en = true, .htltf_en = false, .stbc_htltf2_en = false,
        .ltf_merge_en = false, .channel_filter_en = false,
        .manu_scale = false, .shift = 0, .dump_ack_en = false,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(on_csi, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
}

void app_main(void)
{
    csi_queue = xQueueCreate(CSI_QUEUE_DEPTH, sizeof(csi_record_t));
    message_queue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(probe_t));
    tx_done_queue = xQueueCreate(1, sizeof(esp_now_send_status_t));
    if (!csi_queue || !message_queue || !tx_done_queue) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    start_radio();
    boot_id = esp_random();
    session_id = esp_random();

    printf("\nREADY board=%c, channel=%d, boot=0x%08" PRIx32 "\n",
           'A' + CSI_NODE_ID - 1, CONFIG_CSI_CHANNEL, boot_id);
    printf("Local MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           local_mac[0], local_mac[1], local_mac[2], local_mac[3], local_mac[4], local_mac[5]);
    printf("Peer MAC:  %02x:%02x:%02x:%02x:%02x:%02x\n",
           peer_mac[0], peer_mac[1], peer_mac[2], peer_mac[3], peer_mac[4], peer_mac[5]);
    printf("CSI enabled; %s\n", CSI_NODE_ID == 1
           ? "A initiates request/reply cycles." : "B waits for requests and replies.");
    printf("type,rx_board,tx_board,kind,session,sequence,rx_boot,radio_time_us,"
           "uptime_us,rssi,channel,len,first_word_invalid,data\n");

    if (xTaskCreate(csi_output_task, "csi_output", 4096, NULL, 1, NULL) != pdPASS ||
        xTaskCreate(exchange_task, "csi_exchange", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    for (;;) {
        vTaskDelay(ticks_for_ms(2000));
        counters_t c = snapshot_counters();
        printf("# STATUS %c: tx_ok=%" PRIu32 " tx_fail=%" PRIu32
               " send_errors=%" PRIu32 " messages_rx=%" PRIu32 " csi_rx=%" PRIu32
               " csi_dropped=%" PRIu32 " rx_dropped=%" PRIu32 " unlabelled=%" PRIu32
               " oversize=%" PRIu32 " reply_timeouts=%" PRIu32 " cycles=%" PRIu32 "\n",
               'A' + CSI_NODE_ID - 1, c.tx_ok, c.tx_fail, c.send_errors,
               c.messages_rx, c.csi_rx, c.csi_queue_drops, c.rx_queue_drops,
               c.csi_unlabelled, c.csi_oversize, c.reply_timeouts, c.cycles);
    }
}

#endif /* !CSI_HOST_TEST */
