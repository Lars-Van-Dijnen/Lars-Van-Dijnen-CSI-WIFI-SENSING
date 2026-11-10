#include "csi_parse.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int csv_fields(const char *src, char *dst, size_t capacity, char **fields)
{
    size_t used = 0;
    int count = 0;
    for (;;) {
        int quoted;
        if (count >= CSI_FIELD_COUNT || used >= capacity) return -1;
        fields[count++] = dst + used;
        quoted = *src == '"';
        if (quoted) ++src;
        for (;;) {
            char c = *src;
            if (quoted && c == '"') {
                if (src[1] == '"') { ++src; c = *src; }
                else {
                    ++src;
                    if (*src != ',' && *src != '\0') return -1;
                    break;
                }
            } else if (!quoted && (c == ',' || c == '\0')) break;
            else if (c == '\0' || c == '\r' || c == '\n' || (!quoted && c == '"')) return -1;
            if (used + 1 >= capacity) return -1;
            dst[used++] = c;
            ++src;
        }
        if (used >= capacity) return -1;
        dst[used++] = '\0';
        if (*src == '\0') break;
        ++src;
    }
    return count == CSI_FIELD_COUNT ? 0 : -1;
}

static int unsigned_number(const char *s, int base, uint64_t maximum)
{
    char *end;
    unsigned long long value;
    if (!*s || *s == '-' || *s == '+' || isspace((unsigned char)*s)) return 0;
    errno = 0;
    value = strtoull(s, &end, base);
    return !errno && *end == '\0' && value <= maximum;
}

static int signed_number(const char *s, long minimum, long maximum)
{
    char *end;
    long value;
    if (!*s || isspace((unsigned char)*s)) return 0;
    errno = 0;
    value = strtol(s, &end, 10);
    return !errno && *end == '\0' && value >= minimum && value <= maximum;
}

static int iq_array(const char *s, unsigned expected)
{
    unsigned count = 0;
    if (*s++ != '[') return 0;
    for (;;) {
        char *end;
        long value;
        while (isspace((unsigned char)*s)) ++s;
        if (!*s || *s == ']' || *s == '+') return 0;
        errno = 0;
        value = strtol(s, &end, 10);
        if (end == s || errno || value < -128 || value > 127) return 0;
        s = end;
        ++count;
        if (count > expected) return 0;
        while (isspace((unsigned char)*s)) ++s;
        if (*s == ']') {
            ++s;
            while (isspace((unsigned char)*s)) ++s;
            return *s == '\0' && count == expected;
        }
        if (*s++ != ',') return 0;
    }
}

int csi_validate(const char *line, char *scratch, size_t scratch_size)
{
    char *f[CSI_FIELD_COUNT];
    unsigned length;
    if (strncmp(line, "CSI,", 4) != 0) return 0;
    if (strlen(line) >= CSI_LINE_CAP || csv_fields(line, scratch, scratch_size, f)) return -1;
    if (strcmp(f[0], "CSI") || strcmp(f[3], "PROBE")) return -1;
    if (strlen(f[1]) != 1 || strlen(f[2]) != 1 ||
        f[1][0] < 'A' || f[1][0] > 'C' || f[2][0] < 'A' || f[2][0] > 'C' ||
        f[1][0] == f[2][0]) return -1;
    if (!unsigned_number(f[4], 16, UINT32_MAX) ||
        !unsigned_number(f[5], 10, UINT32_MAX) ||
        !unsigned_number(f[6], 16, UINT32_MAX) ||
        !unsigned_number(f[7], 10, UINT32_MAX) ||
        !unsigned_number(f[8], 10, INT64_MAX) ||
        !signed_number(f[9], -128, 127) ||
        !unsigned_number(f[10], 10, 14) || strtoul(f[10], NULL, 10) < 1 ||
        !unsigned_number(f[11], 10, 128) ||
        (strcmp(f[12], "0") && strcmp(f[12], "1"))) return -1;
    length = (unsigned)strtoul(f[11], NULL, 10);
    if (length < 2 || length % 2 || !iq_array(f[13], length)) return -1;
    return 1;
}

double csi_elapsed(int64_t observed, int64_t origin, int64_t frequency)
{
    if (frequency <= 0) return 0.0;
    /* Subtract integers BEFORE conversion; absolute QPC values can be large. */
    return (double)(observed - origin) / (double)frequency;
}
