/*
 * Three ESP32-S3 boards taking turns transmitting CSI probes -- ESP-IDF 6.1.
 *
 * A coordinates the order: A broadcasts, then grants B a turn, then grants C.
 * Each probe is received/measured by the other two boards. Control grants are
 * excluded from CSI output. Default: 20 ms per turn, about 60 ms per round.
 * UART output is 460800 baud; capture_csv.py writes received records on the PC.
 * Flash the supplied board_a, board_b and board_c projects onto three boards.
 * All radio callbacks copy into queues; worker tasks transmit and print.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* 1. Protocol and data helpers. These also run in the host-side tests. */
enum { NODE_COUNT = 3, WIRE_SIZE = 16, MSG_GRANT = 1, MSG_PROBE = 2,
       CSI_MAX_BYTES = 128, CSI_QUEUE_DEPTH = 64, GRANT_QUEUE_DEPTH = 8 };

/* A separate MAC prefix keeps this version distinct from the two-board demo. */
static const uint8_t node_macs[NODE_COUNT][6] = {
    {0x02, 0x43, 0x53, 0x49, 0x03, 1},
    {0x02, 0x43, 0x53, 0x49, 0x03, 2},
    {0x02, 0x43, 0x53, 0x49, 0x03, 3},
};
static const uint8_t broadcast_mac[6] = {255, 255, 255, 255, 255, 255};

typedef struct {
    uint8_t kind, sender, turn;
    uint32_t session, sequence;
} packet_t;

typedef struct {
    bool seen;
    uint32_t session, sequence;
} grant_state_t;

typedef struct {
    packet_t packet;
    uint32_t radio_time_us;
    int64_t uptime_us;
    int8_t rssi;
    uint8_t channel;
    uint16_t len;
    bool first_word_invalid;
    int8_t data[CSI_MAX_BYTES];
} csi_record_t;

static uint8_t node_from_mac(const uint8_t *mac)
{
    if (mac) {
        for (uint8_t id = 1; id <= NODE_COUNT; ++id) {
            if (memcmp(mac, node_macs[id - 1], 6) == 0) { return id; }
        }
    }
    return 0;
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_u32_le(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) { p[i] = (uint8_t)(value >> (8 * i)); }
}

static void encode_packet(uint8_t out[WIRE_SIZE], const packet_t *packet)
{
    memcpy(out, "S3T3", 4);
    out[4] = 1;
    out[5] = packet->kind;
    out[6] = packet->sender;
    out[7] = packet->turn;
    write_u32_le(out + 8, packet->session);
    write_u32_le(out + 12, packet->sequence);
}

static bool decode_packet(const uint8_t *data, size_t len,
                          uint8_t expected_sender, packet_t *out)
{
    if (!data || !out || len != WIRE_SIZE || memcmp(data, "S3T3", 4) != 0 ||
        data[4] != 1 || expected_sender < 1 || expected_sender > NODE_COUNT ||
        data[6] != expected_sender || data[7] < 1 || data[7] > NODE_COUNT) {
        return false;
    }
    const bool valid_grant = data[5] == MSG_GRANT && data[6] == 1 && data[7] != 1;
    const bool valid_probe = data[5] == MSG_PROBE && data[6] == data[7];
    if (!valid_grant && !valid_probe) { return false; }
    *out = (packet_t) {.kind = data[5], .sender = data[6], .turn = data[7],
                      .session = read_u32_le(data + 8),
                      .sequence = read_u32_le(data + 12)};
    return true;
}

/* CSI exposes the action-frame body. Our unencrypted 16-byte ESP-NOW message
 * begins after a 15-byte vendor-action prefix. Validate before accessing it.
 */
static bool decode_csi_probe(const uint8_t *body, size_t len,
                             uint8_t sender, packet_t *out)
{
    static const uint8_t oui[3] = {0x18, 0xfe, 0x34};
    if (!body || !out || len < 15 + WIRE_SIZE || body[0] != 127 ||
        memcmp(body + 1, oui, 3) != 0 || body[8] != 221 ||
        memcmp(body + 10, oui, 3) != 0 || body[13] != 4 ||
        body[9] != 5 + WIRE_SIZE || body[9] > len - 10 ||
        ((body[14] & 0x0f) != 1 && (body[14] & 0x0f) != 2) ||
        (body[14] & 0x10) != 0) {
        return false;
    }
    packet_t packet;
    if (!decode_packet(body + 15, WIRE_SIZE, sender, &packet) ||
        packet.kind != MSG_PROBE) {
        return false;
    }
    *out = packet;
    return true;
}

/* Reject repeated/older rounds, including across uint32_t sequence wrap. */
static bool accept_grant(grant_state_t *state, const packet_t *packet,
                         uint8_t node, int64_t arrived_us, int64_t now_us,
                         int64_t freshness_us)
{
    if (!state || !packet || node < 2 || node > NODE_COUNT ||
        packet->kind != MSG_GRANT || packet->sender != 1 || packet->turn != node ||
        arrived_us < 0 || now_us < arrived_us || freshness_us < 0 ||
        now_us - arrived_us > freshness_us) {
        return false;
    }
    if (state->seen && state->session == packet->session) {
        uint32_t delta = packet->sequence - state->sequence;
        if (delta == 0 || delta >= UINT32_C(0x80000000)) { return false; }
    }
    *state = (grant_state_t) {.seen = true, .session = packet->session,
                             .sequence = packet->sequence};
    return true;
}

static void advance_turn(uint8_t *turn, uint32_t *sequence)
{
    if (*turn == NODE_COUNT) { *turn = 1; ++*sequence; }
    else { ++*turn; }
}

/* With a late radio completion, leave extra time for the peer's permitted
 * response instead of immediately granting the next board a turn.
 */
static int64_t turn_deadline(int64_t started_us, int64_t completed_us,
                             int64_t turn_us, int64_t quiet_us)
{
    const int64_t nominal = started_us + turn_us;
    const int64_t quiet_end = completed_us + quiet_us;
    return nominal > quiet_end ? nominal : quiet_end;
}

/* Format one complete line so logging from other tasks cannot split a row. */
static size_t format_csi(char *line, size_t capacity, const csi_record_t *record,
                         uint8_t receiver, uint32_t boot_id)
{
    if (!line || !record || capacity == 0 || receiver < 1 || receiver > NODE_COUNT ||
        record->packet.kind != MSG_PROBE || record->packet.sender < 1 ||
        record->packet.sender > NODE_COUNT || record->packet.sender == receiver ||
        record->packet.turn != record->packet.sender ||
        record->len == 0 || record->len > CSI_MAX_BYTES) {
        return 0;
    }
    int n = snprintf(line, capacity,
        "CSI,%c,%c,PROBE,0x%08" PRIx32 ",%" PRIu32 ",0x%08" PRIx32
        ",%" PRIu32 ",%" PRId64 ",%d,%u,%u,%u,\"[",
        'A' + receiver - 1, 'A' + record->packet.sender - 1,
        record->packet.session, record->packet.sequence, boot_id,
        record->radio_time_us, record->uptime_us, (int)record->rssi,
        (unsigned)record->channel, (unsigned)record->len,
        (unsigned)record->first_word_invalid);
    if (n < 0 || (size_t)n >= capacity) { return 0; }
    size_t used = (size_t)n;
    for (unsigned i = 0; i < record->len; ++i) {
        n = snprintf(line + used, capacity - used, "%s%d",
                     i ? "," : "", (int)record->data[i]);
        if (n < 0 || (size_t)n >= capacity - used) { return 0; }
        used += (size_t)n;
    }
    if (capacity - used < 4) { return 0; }
    memcpy(line + used, "]\"\n", 4);
    return used + 3;
}

#ifndef CSI_HOST_TEST

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
#error "Select ESP32-S3: idf.py set-target esp32s3"
#endif
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 1, 0)
#error "This source uses the ESP-IDF 6.1 ESP-NOW callback API."
#endif
#if !defined(CONFIG_ESP_WIFI_CSI_ENABLED) || !CONFIG_ESP_WIFI_CSI_ENABLED
#error "Enable Wi-Fi CSI in menuconfig."
#endif

/* 2. Settings: the three supplied projects select A/B/C automatically. */
#ifndef CSI_NODE_ID
#if defined(CONFIG_CSI_BOARD_C) && CONFIG_CSI_BOARD_C
#define CSI_NODE_ID 3
#elif defined(CONFIG_CSI_BOARD_B) && CONFIG_CSI_BOARD_B
#define CSI_NODE_ID 2
#else
#define CSI_NODE_ID 1
#endif
#endif
#if CSI_NODE_ID < 1 || CSI_NODE_ID > 3
#error "CSI_NODE_ID must be 1 (A), 2 (B), or 3 (C)."
#endif
#ifndef CONFIG_CSI_CHANNEL
#define CONFIG_CSI_CHANNEL 6
#endif
#ifndef CONFIG_CSI_TURN_MS
#define CONFIG_CSI_TURN_MS 20
#endif
#ifndef CONFIG_CSI_REPLY_DELAY_MS
#define CONFIG_CSI_REPLY_DELAY_MS 2
#endif

typedef struct { packet_t packet; int64_t arrived_us; } grant_event_t;
typedef struct {
    uint32_t tx_ok, tx_fail, send_errors, probes_tx, probes_rx;
    uint32_t grants_rx, grants_dropped, grants_rejected;
    uint32_t csi_from[NODE_COUNT], csi_dropped, csi_unlabelled, csi_oversize;
    uint32_t rounds, turn_overruns;
} counters_t;

static const char *TAG = "csi_three";
static QueueHandle_t csi_queue, grant_queue, tx_done_queue;
static counters_t counters;
static portMUX_TYPE counter_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t boot_id, session_id;

static void count_one(uint32_t *counter)
{
    portENTER_CRITICAL(&counter_lock);
    ++*counter;
    portEXIT_CRITICAL(&counter_lock);
}

static counters_t snapshot_counters(void)
{
    portENTER_CRITICAL(&counter_lock);
    counters_t copy = counters;
    portEXIT_CRITICAL(&counter_lock);
    return copy;
}

static TickType_t ticks_for_ms(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks ? ticks : 1;
}

static int64_t grant_freshness_us(void)
{
    return (int64_t)CONFIG_CSI_TURN_MS * 1000 / 4;
}

static void wait_until(int64_t deadline_us)
{
    int64_t left = deadline_us - esp_timer_get_time();
    if (left > 0) { vTaskDelay(ticks_for_ms((uint32_t)((left + 999) / 1000))); }
}

/* 3. Driver callbacks: copy/validate data; never wait or print here. */
static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    count_one(status == ESP_NOW_SEND_SUCCESS ? &counters.tx_ok : &counters.tx_fail);
    xQueueOverwrite(tx_done_queue, &status);
}

static void on_message(const esp_now_recv_info_t *info,
                       const uint8_t *data, int len)
{
    if (!info || !info->des_addr || len < 0) { return; }
    const uint8_t sender = node_from_mac(info->src_addr);
    packet_t packet;
    if (sender == 0 || sender == CSI_NODE_ID ||
        !decode_packet(data, (size_t)len, sender, &packet)) {
        return;
    }
    if (packet.kind == MSG_PROBE && memcmp(info->des_addr, broadcast_mac, 6) == 0) {
        count_one(&counters.probes_rx);
    } else if (CSI_NODE_ID != 1 && packet.kind == MSG_GRANT &&
               packet.turn == CSI_NODE_ID &&
               memcmp(info->des_addr, node_macs[CSI_NODE_ID - 1], 6) == 0) {
        grant_event_t event = {.packet = packet, .arrived_us = esp_timer_get_time()};
        count_one(&counters.grants_rx);
        if (xQueueSend(grant_queue, &event, 0) != pdTRUE) {
            count_one(&counters.grants_dropped);
        }
    }
}

static void on_csi(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;
    if (!info || !info->buf || info->len == 0 ||
        memcmp(info->dmac, broadcast_mac, 6) != 0) {
        return;
    }
    const uint8_t sender = node_from_mac(info->mac);
    if (sender == 0 || sender == CSI_NODE_ID) { return; }
    packet_t packet;
    if (!decode_csi_probe(info->payload, info->payload_len, sender, &packet)) {
        count_one(&counters.csi_unlabelled);
        return;
    }
    if (info->len > CSI_MAX_BYTES) { count_one(&counters.csi_oversize); return; }
    csi_record_t record = {
        .packet = packet, .radio_time_us = info->rx_ctrl.timestamp,
        .uptime_us = esp_timer_get_time(), .rssi = info->rx_ctrl.rssi,
        .channel = info->rx_ctrl.channel, .len = info->len,
        .first_word_invalid = info->first_word_invalid,
    };
    memcpy(record.data, info->buf, info->len);
    count_one(&counters.csi_from[sender - 1]);
    if (xQueueSend(csi_queue, &record, 0) != pdTRUE) { count_one(&counters.csi_dropped); }
}

/* 4. Serialized sending: one outstanding send per board, wait for completion. */
static bool send_packet(const uint8_t *destination, const packet_t *packet)
{
    uint8_t wire[WIRE_SIZE];
    encode_packet(wire, packet);
    esp_now_send_status_t status;
    while (xQueueReceive(tx_done_queue, &status, 0) == pdTRUE) { }
    esp_err_t err = esp_now_send(destination, wire, sizeof(wire));
    if (err != ESP_OK) {
        count_one(&counters.send_errors);
        ESP_LOGW(TAG, "esp_now_send: %s", esp_err_to_name(err));
        return false;
    }
    if (xQueueReceive(tx_done_queue, &status, ticks_for_ms(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "Missing send callback; restarting to reset send state.");
        esp_restart();
        return false;
    }
    bool ok = status == ESP_NOW_SEND_SUCCESS;
    if (ok && packet->kind == MSG_PROBE) { count_one(&counters.probes_tx); }
    return ok;
}

static void broadcast_probe(uint32_t session, uint32_t sequence)
{
    packet_t packet = {.kind = MSG_PROBE, .sender = CSI_NODE_ID,
                       .turn = CSI_NODE_ID, .session = session, .sequence = sequence};
    (void)send_packet(broadcast_mac, &packet);
}

static void exchange_task(void *arg)
{
    (void)arg;
    if (CSI_NODE_ID == 1) {
        uint8_t turn = 1;
        uint32_t sequence = 0;
        for (;;) {
            const int64_t started = esp_timer_get_time();
            if (turn == 1) {
                count_one(&counters.rounds);
                broadcast_probe(session_id, sequence);
            } else {
                packet_t grant = {.kind = MSG_GRANT, .sender = 1, .turn = turn,
                                  .session = session_id, .sequence = sequence};
                (void)send_packet(node_macs[turn - 1], &grant);
            }
            const int64_t completed = esp_timer_get_time();
            const int64_t quiet_us = turn == 1 ? 5000 : grant_freshness_us() +
                                     (int64_t)CONFIG_CSI_REPLY_DELAY_MS * 1000 + 10000;
            const int64_t turn_us = (int64_t)CONFIG_CSI_TURN_MS * 1000;
            int64_t deadline = turn_deadline(started, completed, turn_us, quiet_us);
            if (deadline > started + turn_us) { count_one(&counters.turn_overruns); }
            wait_until(deadline);
            advance_turn(&turn, &sequence);
        }
    } else {
        grant_state_t previous = {0};
        for (;;) {
            grant_event_t event;
            if (xQueueReceive(grant_queue, &event, portMAX_DELAY) != pdTRUE) { continue; }
            if (!accept_grant(&previous, &event.packet, CSI_NODE_ID,
                              event.arrived_us, esp_timer_get_time(), grant_freshness_us())) {
                count_one(&counters.grants_rejected);
                continue;
            }
            /* Let the grant's Wi-Fi ACK finish before broadcasting the probe. */
            vTaskDelay(ticks_for_ms(CONFIG_CSI_REPLY_DELAY_MS));
            const int64_t latest = event.arrived_us + grant_freshness_us() +
                                   (int64_t)CONFIG_CSI_REPLY_DELAY_MS * 1000;
            if (esp_timer_get_time() > latest) {
                count_one(&counters.grants_rejected);
                continue;
            }
            broadcast_probe(event.packet.session, event.packet.sequence);
        }
    }
}

/* 5. A lower-priority task prints complete CSV records. */
static void csi_output_task(void *arg)
{
    (void)arg;
    csi_record_t record;
    char line[1024];
    for (;;) {
        if (xQueueReceive(csi_queue, &record, portMAX_DELAY) == pdTRUE &&
            format_csi(line, sizeof(line), &record, CSI_NODE_ID, boot_id) != 0) {
            fputs(line, stdout);
        }
    }
}

/* 6. Radio setup: same channel, broadcast peer, HT20 MCS0, LLTF-only CSI. */
static void add_peer(const uint8_t *mac)
{
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = CONFIG_CSI_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    esp_now_rate_config_t rate = {.phymode = WIFI_PHY_MODE_HT20,
                                 .rate = WIFI_PHY_RATE_MCS0_LGI,
                                 .ersu = false, .dcm = false};
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(mac, &rate));
}

static void start_radio(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, node_macs[CSI_NODE_ID - 1]));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_CSI_CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    add_peer(broadcast_mac);
    for (uint8_t node = 1; node <= NODE_COUNT; ++node) {
        if (node != CSI_NODE_ID) { add_peer(node_macs[node - 1]); }
    }
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_message));
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
    grant_queue = xQueueCreate(GRANT_QUEUE_DEPTH, sizeof(grant_event_t));
    tx_done_queue = xQueueCreate(1, sizeof(esp_now_send_status_t));
    if (!csi_queue || !grant_queue || !tx_done_queue) { ESP_ERROR_CHECK(ESP_ERR_NO_MEM); }
    start_radio();
    boot_id = esp_random();
    session_id = esp_random();
    printf("\nREADY board=%c, channel=%d, turn_ms=%d, boot=0x%08" PRIx32 "\n",
           'A' + CSI_NODE_ID - 1, CONFIG_CSI_CHANNEL, CONFIG_CSI_TURN_MS, boot_id);
    printf("CSI enabled; %s\n", CSI_NODE_ID == 1
           ? "A coordinates turns A -> B -> C." : "Waiting for Board A to grant a turn.");
    printf("Approximate round=%d ms; each link=%.2f Hz; each receiver=%.2f rows/s.\n",
           3 * CONFIG_CSI_TURN_MS, 1000.0 / (3 * CONFIG_CSI_TURN_MS),
           2000.0 / (3 * CONFIG_CSI_TURN_MS));
    printf("type,rx_board,tx_board,kind,session,sequence,rx_boot,radio_time_us,"
           "uptime_us,rssi,channel,len,first_word_invalid,data\n");
    if (xTaskCreate(csi_output_task, "csi_output", 4096, NULL, 1, NULL) != pdPASS ||
        xTaskCreate(exchange_task, "csi_turns", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    for (;;) {
        vTaskDelay(ticks_for_ms(2000));
        counters_t c = snapshot_counters();
        printf("# STATUS %c: probes_tx=%" PRIu32 " probes_rx=%" PRIu32
               " csi_A=%" PRIu32 " csi_B=%" PRIu32 " csi_C=%" PRIu32
               " tx_ok=%" PRIu32 " tx_fail=%" PRIu32 " send_errors=%" PRIu32
               " grants_rx=%" PRIu32 " grants_dropped=%" PRIu32
               " grants_rejected=%" PRIu32 " csi_dropped=%" PRIu32
               " unlabelled=%" PRIu32 " oversize=%" PRIu32
               " rounds=%" PRIu32 " turn_overruns=%" PRIu32 "\n",
               'A' + CSI_NODE_ID - 1, c.probes_tx, c.probes_rx,
               c.csi_from[0], c.csi_from[1], c.csi_from[2], c.tx_ok, c.tx_fail,
               c.send_errors, c.grants_rx, c.grants_dropped, c.grants_rejected,
               c.csi_dropped, c.csi_unlabelled, c.csi_oversize, c.rounds, c.turn_overruns);
    }
}

#endif /* !CSI_HOST_TEST */
