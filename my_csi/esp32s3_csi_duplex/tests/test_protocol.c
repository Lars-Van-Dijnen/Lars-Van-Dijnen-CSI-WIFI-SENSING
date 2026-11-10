/* Host-side checks of the exact packet parser used by the firmware. */
#define CSI_HOST_TEST
#include "../esp32s3_csi_duplex.c"
#include <assert.h>
#include <stdlib.h>

static void check_probe(const probe_t *p, const probe_t *q)
{
    assert(p->sender == q->sender && p->kind == q->kind);
    assert(p->session == q->session && p->sequence == q->sequence);
}

int main(void)
{
    uint8_t wire[PROBE_SIZE];
    probe_t request = {1, PROBE_REQUEST, 0x89abcdef, 0x12345678};
    probe_t result;
    encode_probe(wire, &request);
    assert(wire[8] == 0xef && wire[11] == 0x89);
    assert(wire[12] == 0x78 && wire[15] == 0x12);
    assert(decode_probe(wire, sizeof(wire), 1, &result));
    check_probe(&request, &result);
    assert(!decode_probe(wire, sizeof(wire), 2, &result));
    assert(!decode_probe(NULL, sizeof(wire), 1, &result));
    assert(!decode_probe(wire, sizeof(wire), 1, NULL));
    for (size_t len = 0; len < PROBE_SIZE; ++len) {
        assert(!decode_probe(wire, len, 1, &result));
    }
    assert(!decode_probe(wire, PROBE_SIZE + 1, 1, &result));
    for (unsigned i = 0; i < 8; ++i) {
        uint8_t corrupt[PROBE_SIZE];
        memcpy(corrupt, wire, sizeof(wire));
        corrupt[i] ^= 0x80;
        assert(!decode_probe(corrupt, sizeof(corrupt), 1, &result));
    }
    wire[6] = PROBE_REPLY;
    assert(!decode_probe(wire, sizeof(wire), 1, &result));

    probe_t reply = {2, PROBE_REPLY, UINT32_MAX, 0};
    encode_probe(wire, &reply);
    assert(decode_probe(wire, sizeof(wire), 2, &result));
    check_probe(&reply, &result);

    uint8_t frame[15 + PROBE_SIZE + 4] = {
        127, 0x18, 0xfe, 0x34, 0, 0, 0, 0,
        221, 5 + PROBE_SIZE, 0x18, 0xfe, 0x34, 4, 2,
    };
    memcpy(frame + 15, wire, sizeof(wire));
    assert(decode_csi_probe(frame, sizeof(frame), 2, &result));
    check_probe(&reply, &result);
    assert(decode_csi_probe(frame, 15 + PROBE_SIZE, 2, &result));
    assert(!decode_csi_probe(frame, sizeof(frame), 1, &result));
    assert(!decode_csi_probe(NULL, sizeof(frame), 2, &result));
    assert(!decode_csi_probe(frame, sizeof(frame), 2, NULL));
    for (size_t len = 0; len < 15 + PROBE_SIZE; ++len) {
        /* Exact-size allocations let AddressSanitizer detect overreads. */
        uint8_t *short_frame = malloc(len ? len : 1);
        assert(short_frame);
        memcpy(short_frame, frame, len);
        assert(!decode_csi_probe(short_frame, len, 2, &result));
        free(short_frame);
    }
    frame[14] = 1;
    assert(decode_csi_probe(frame, sizeof(frame), 2, &result));
    frame[14] = 0x12; /* More-data/fragment bit unsupported for this probe. */
    assert(!decode_csi_probe(frame, sizeof(frame), 2, &result));
    frame[14] = 3;
    assert(!decode_csi_probe(frame, sizeof(frame), 2, &result));
    frame[14] = 2;
    const unsigned protected_offsets[] = {0, 1, 2, 3, 8, 9, 10, 11, 12, 13};
    for (unsigned i = 0; i < sizeof(protected_offsets) / sizeof(unsigned); ++i) {
        unsigned at = protected_offsets[i];
        frame[at] ^= 1;
        assert(!decode_csi_probe(frame, sizeof(frame), 2, &result));
        frame[at] ^= 1;
    }
    frame[9] = 255;
    assert(!decode_csi_probe(frame, sizeof(frame), 2, &result));

    /* Deterministic malformed-input sweep, including all boundary lengths. */
    uint32_t rng = 0x73ab912d;
    for (size_t len = 0; len <= 256; ++len) {
        uint8_t *noise = malloc(len ? len : 1);
        assert(noise);
        for (unsigned trial = 0; trial < 32; ++trial) {
            for (size_t i = 0; i < len; ++i) {
                rng = rng * 1664525u + 1013904223u;
                noise[i] = (uint8_t)(rng >> 24);
            }
            (void)decode_csi_probe(noise, len, 2, &result);
            (void)decode_probe(noise, len, 1, &result);
        }
        free(noise);
    }
    puts("Protocol checks passed: encoding, direction, bounds, malformed frames.");
    return 0;
}
