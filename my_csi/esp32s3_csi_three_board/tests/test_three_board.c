/* Checks use the same protocol, turn and CSV functions as the firmware. */
#define CSI_HOST_TEST
#include "../esp32s3_csi_three_board.c"
#include <assert.h>
#include <stdlib.h>

static void check_wire_and_frames(void)
{
    for (uint8_t sender = 1; sender <= NODE_COUNT; ++sender) {
        packet_t p = {.kind = MSG_PROBE, .sender = sender, .turn = sender,
                      .session = UINT32_C(0x89abcdef), .sequence = UINT32_MAX};
        uint8_t wire[WIRE_SIZE];
        encode_packet(wire, &p);
        assert(wire[8] == 0xef && wire[11] == 0x89 && wire[15] == 255);
        packet_t q;
        assert(decode_packet(wire, sizeof(wire), sender, &q));
        assert(q.kind == p.kind && q.sender == p.sender && q.turn == p.turn);
        assert(q.session == p.session && q.sequence == p.sequence);
        for (size_t len = 0; len < WIRE_SIZE; ++len) {
            uint8_t *short_data = malloc(len ? len : 1);
            assert(short_data);
            memcpy(short_data, wire, len);
            assert(!decode_packet(short_data, len, sender, &q));
            free(short_data);
        }
        assert(!decode_packet(wire, WIRE_SIZE + 1, sender, &q));
        assert(!decode_packet(wire, WIRE_SIZE, 0, &q));
        assert(!decode_packet(wire, WIRE_SIZE, 4, &q));
        assert(!decode_packet(wire, WIRE_SIZE, sender % 3 + 1, &q));
        assert(!decode_packet(NULL, WIRE_SIZE, sender, &q));
        assert(!decode_packet(wire, WIRE_SIZE, sender, NULL));
        for (unsigned byte = 0; byte < 8; ++byte) {
            wire[byte] ^= 0x80;
            assert(!decode_packet(wire, WIRE_SIZE, sender, &q));
            wire[byte] ^= 0x80;
        }
        uint8_t body[15 + WIRE_SIZE + 4] = {
            127, 0x18, 0xfe, 0x34, 0, 0, 0, 0,
            221, 5 + WIRE_SIZE, 0x18, 0xfe, 0x34, 4, 2,
        };
        memcpy(body + 15, wire, WIRE_SIZE);
        assert(decode_csi_probe(body, sizeof(body), sender, &q));
        for (size_t len = 0; len < 15 + WIRE_SIZE; ++len) {
            uint8_t *short_body = malloc(len ? len : 1);
            assert(short_body);
            memcpy(short_body, body, len);
            assert(!decode_csi_probe(short_body, len, sender, &q));
            free(short_body);
        }
        const unsigned protected[] = {0, 1, 2, 3, 8, 9, 10, 11, 12, 13};
        for (unsigned i = 0; i < sizeof(protected) / sizeof(unsigned); ++i) {
            body[protected[i]] ^= 1;
            assert(!decode_csi_probe(body, sizeof(body), sender, &q));
            body[protected[i]] ^= 1;
        }
        body[14] = 1;
        assert(decode_csi_probe(body, sizeof(body), sender, &q));
        body[14] = 0x12;
        assert(!decode_csi_probe(body, sizeof(body), sender, &q));
        body[14] = 3;
        assert(!decode_csi_probe(body, sizeof(body), sender, &q));
        body[14] = 2;
        p = (packet_t){.kind = MSG_GRANT, .sender = 1, .turn = 2};
        encode_packet(body + 15, &p);
        assert(!decode_csi_probe(body, sizeof(body), 1, &q)); /* Control is not CSI data. */
    }
    for (uint8_t node = 1; node <= NODE_COUNT; ++node) {
        assert(node_from_mac(node_macs[node - 1]) == node);
    }
    assert(node_from_mac(NULL) == 0 && node_from_mac(broadcast_mac) == 0);
    uint8_t wrong_mac[6];
    memcpy(wrong_mac, node_macs[0], 6);
    wrong_mac[4] = 0;
    assert(node_from_mac(wrong_mac) == 0); /* Old two-board firmware is excluded. */
}

static void check_grants(void)
{
    grant_state_t state = {0};
    packet_t p = {.kind = MSG_GRANT, .sender = 1, .turn = 2,
                  .session = 55, .sequence = UINT32_MAX - 1};
    assert(!accept_grant(&state, &p, 3, 100, 101, 25));
    assert(!accept_grant(&state, &p, 1, 100, 101, 25));
    assert(!accept_grant(&state, &p, 2, 100, 99, 25));
    assert(!accept_grant(&state, &p, 2, 100, 126, 25));
    assert(!state.seen);
    assert(accept_grant(&state, &p, 2, 100, 125, 25));
    assert(!accept_grant(&state, &p, 2, 100, 101, 25)); /* Duplicate. */
    p.sequence--;
    assert(!accept_grant(&state, &p, 2, 100, 101, 25)); /* Older round. */
    p.sequence = UINT32_MAX;
    assert(accept_grant(&state, &p, 2, 100, 101, 25));
    p.sequence = 0;
    assert(accept_grant(&state, &p, 2, 100, 101, 25)); /* Wrap. */
    p.sequence = UINT32_C(0x80000000);
    assert(!accept_grant(&state, &p, 2, 100, 101, 25));
    p.session++;
    p.sequence = 0;
    assert(accept_grant(&state, &p, 2, 100, 101, 25)); /* A restarted. */
    p.sender = 3;
    assert(!accept_grant(&state, &p, 2, 100, 101, 25));
    assert(!accept_grant(NULL, &p, 2, 100, 101, 25));
    assert(!accept_grant(&state, NULL, 2, 100, 101, 25));
    assert(turn_deadline(0, 2000, 100000, 40000) == 100000);
    assert(turn_deadline(0, 90000, 100000, 40000) == 130000);
    /* Fast default: 20 ms turn; 5 ms freshness + 2 ms reply + 10 ms guard.
     * On-time sends keep a 60 ms A+B+C round; late sends retain the guard.
     */
    assert(turn_deadline(0, 2000, 20000, 17000) == 20000);
    assert(turn_deadline(0, 6000, 20000, 17000) == 23000);
    assert(turn_deadline(20000, 22000, 20000, 17000) == 40000);
    assert(turn_deadline(40000, 42000, 20000, 17000) == 60000);
}

/* Protocol-level simulation: loss/duplicates never make the coordinator wait
 * for a token from a missing peer. This is not a simulation of the RF hardware.
 */
static void check_rounds(bool missing_b, bool lose_grants)
{
    grant_state_t states[3] = {0};
    unsigned records[3][3] = {{0}};
    uint8_t turn = 1;
    uint32_t sequence = 0;
    const uint32_t rounds = 40;
    for (unsigned slot = 0; slot < rounds * 3; ++slot) {
        assert(turn == slot % 3 + 1 && sequence == slot / 3);
        bool transmit = turn == 1;
        bool lost = lose_grants && sequence % 3 == 0;
        if (turn != 1 && !(missing_b && turn == 2) && !lost) {
            packet_t grant = {.kind = MSG_GRANT, .sender = 1, .turn = turn,
                              .session = 77, .sequence = sequence};
            int64_t now = (int64_t)slot * 20000;
            transmit = accept_grant(&states[turn - 1], &grant, turn, now, now + 1000, 5000);
            assert(!accept_grant(&states[turn - 1], &grant, turn, now, now + 2000, 5000));
        }
        if (transmit) {
            for (unsigned rx = 1; rx <= 3; ++rx) {
                if (rx != turn && !(missing_b && rx == 2)) { records[rx - 1][turn - 1]++; }
            }
        }
        advance_turn(&turn, &sequence);
    }
    assert(sequence == rounds && turn == 1);
    for (unsigned node = 0; node < 3; ++node) { assert(records[node][node] == 0); }
    assert(records[2][0] == rounds); /* C continues receiving A even if B is absent. */
    assert(records[0][2] > 0);       /* A continues receiving C after lost grants. */
    if (!missing_b && !lose_grants) {
        for (unsigned rx = 0; rx < 3; ++rx) {
            for (unsigned tx = 0; tx < 3; ++tx) {
                if (rx != tx) { assert(records[rx][tx] == rounds); }
            }
        }
    }
    if (missing_b) { assert(records[0][1] == 0 && records[2][1] == 0); }
    sequence = UINT32_MAX;
    turn = 3;
    advance_turn(&turn, &sequence);
    assert(turn == 1 && sequence == 0);
}

static void check_csv(void)
{
    csi_record_t r = {.packet = {.kind = MSG_PROBE, .sender = 2, .turn = 2,
                                .session = UINT32_MAX, .sequence = UINT32_MAX},
                      .radio_time_us = UINT32_MAX, .uptime_us = INT64_MAX,
                      .rssi = -128, .channel = 11, .len = CSI_MAX_BYTES,
                      .first_word_invalid = true};
    for (unsigned i = 0; i < CSI_MAX_BYTES; ++i) { r.data[i] = -128; }
    char line[1024];
    size_t n = format_csi(line, sizeof(line), &r, 1, UINT32_MAX);
    assert(n > 0 && n == strlen(line) && n < sizeof(line));
    assert(strncmp(line, "CSI,A,B,PROBE,0xffffffff,4294967295,", 35) == 0);
    assert(strcmp(line + n - 3, "]\"\n") == 0);
    assert(strstr(line, ",11,128,1,\"[-128,") != NULL);
    for (size_t size = 0; size <= n; ++size) {
        char *small = malloc(size ? size : 1);
        assert(small);
        assert(format_csi(small, size, &r, 1, 123) == 0);
        free(small);
    }
    assert(format_csi(line, sizeof(line), &r, 2, 0) == 0);
    assert(format_csi(line, sizeof(line), &r, 4, 0) == 0);
    assert(format_csi(NULL, sizeof(line), &r, 1, 0) == 0);
    assert(format_csi(line, sizeof(line), NULL, 1, 0) == 0);
    r.len++;
    assert(format_csi(line, sizeof(line), &r, 1, 0) == 0);
}

/* Real firmware-formatted rows for the Python recorder's integration tests. */
static void emit_csv_fixture(void)
{
    for (uint8_t rx = 1; rx <= NODE_COUNT; ++rx) {
        for (uint8_t tx = 1; tx <= NODE_COUNT; ++tx) {
            if (rx == tx) { continue; }
            csi_record_t r = {
                .packet = {.kind = MSG_PROBE, .sender = tx, .turn = tx,
                           .session = UINT32_MAX, .sequence = 42},
                .radio_time_us = UINT32_MAX, .uptime_us = INT64_C(5000000000),
                .rssi = -55, .channel = 6, .len = CSI_MAX_BYTES,
                .first_word_invalid = tx == 2,
            };
            for (unsigned i = 0; i < CSI_MAX_BYTES; ++i) {
                r.data[i] = (int8_t)((int)(i + rx + tx) % 256 - 128);
            }
            char line[1024];
            assert(format_csi(line, sizeof(line), &r, rx, rx) != 0);
            fputs(line, stdout);
        }
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--emit-csv") == 0) {
        emit_csv_fixture();
        return 0;
    }
    check_wire_and_frames();
    check_grants();
    check_rounds(false, false);
    check_rounds(true, false);
    check_rounds(false, true);
    check_rounds(true, true);
    check_csv();
    uint32_t rng = UINT32_C(0xdeadbeef);
    for (size_t len = 0; len <= 256; ++len) {
        uint8_t *noise = malloc(len ? len : 1);
        assert(noise);
        for (unsigned trial = 0; trial < 32; ++trial) {
            for (size_t i = 0; i < len; ++i) {
                rng = rng * 1664525u + 1013904223u;
                noise[i] = (uint8_t)(rng >> 24);
            }
            packet_t packet;
            (void)decode_packet(noise, len, 1, &packet);
            (void)decode_csi_probe(noise, len, 2, &packet);
        }
        free(noise);
    }
    puts("PASS: packet bounds, MAC identities, grants/replays/wrap, A-B-C rounds,");
    puts("      six directions, missing B, lost grants, CSV bounds and malformed input.");
    return 0;
}
