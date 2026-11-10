#ifndef CSI_PARSE_H
#define CSI_PARSE_H

#include <stddef.h>
#include <stdint.h>

#define CSI_LINE_CAP 4096
#define CSI_FIELD_COUNT 14

/* 1 = validated CSI row, 0 = other console output, -1 = damaged CSI row.
   scratch receives decoded CSV fields; line itself is never changed. */
int csi_validate(const char *line, char *scratch, size_t scratch_size);

/* QPC ticks to seconds. Kept portable so the arithmetic can be tested. */
double csi_elapsed(int64_t observed, int64_t origin, int64_t frequency);

#endif
