#include "csi_parse.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static char scratch[CSI_LINE_CAP];

static void check(const char *row, int expected)
{
    int actual = csi_validate(row, scratch, sizeof scratch);
    if (actual != expected) fprintf(stderr, "Unexpected parse result %d: %s\n", actual, row);
    assert(actual == expected);
}

int main(void)
{
    char full[CSI_LINE_CAP], long_row[CSI_LINE_CAP + 100];
    size_t used;
    int i;
    check("READY board=A", 0);
    check("# STATUS ...", 0);
    check("CSI,A,B,PROBE,abcdef01,1,1234,123,456,-60,6,4,0,\"[-128,127,0,-1]\"", 1);
    check("CSI,C,A,PROBE,FFFFFFFF,4294967295,FFFFFFFF,4294967295,9223372036854775807,-128,14,2,1,\"[1,2]\"", 1);
    check("CSI,A,A,PROBE,a,1,b,2,3,-60,6,2,0,\"[1,2]\"", -1);
    check("CSI,D,B,PROBE,a,1,b,2,3,-60,6,2,0,\"[1,2]\"", -1);
    check("CSI,A,B,OTHER,a,1,b,2,3,-60,6,2,0,\"[1,2]\"", -1);
    check("CSI,A,B,PROBE,a,1,b,2,3,-60,0,2,0,\"[1,2]\"", -1);
    check("CSI,A,B,PROBE,a,1,b,2,3,-60,6,3,0,\"[1,2,3]\"", -1);
    check("CSI,A,B,PROBE,a,1,b,2,3,-60,6,4,0,\"[1,2]\"", -1);
    check("CSI,A,B,PROBE,a,1,b,2,3,-60,6,2,0,\"[128,2]\"", -1);
    check("CSI,A,B,PROBE,a,1,b,2,3,-60,6,2,0,\"[1,-129]\"", -1);
    check("CSI,A,B,PROBE,a,1,b,2,3,-60,6,2,0,\"[1,2,]\"", -1);
    check("CSI,A,B,PROBE,a,1,b,2,3,-60,6,2,0,\"[1.0,2]\"", -1);
    check("CSI,A,B,PROBE,a,1,b,2,3,-60,6,2,0,\"[1,2]\"junk", -1);
    check("CSI,A,B,PROBE,a,1,b,2,3,-60,6,2,0,\"[1,2]", -1);
    check("CSI,A,B,PROBE,a,1,b,2,3,-60,6,2,0,\"[1,2]\",extra", -1);
    check("CSI,A,B,PROBE,a,1,b,2,3,-60,6,2,2,\"[1,2]\"", -1);
    check("CSI,A,B,PROBE,a,4294967296,b,2,3,-60,6,2,0,\"[1,2]\"", -1);
    check("CSI,A,B,PROBE,a,1,b,-2,3,-60,6,2,0,\"[1,2]\"", -1);
    check("CSI,A,B,PROBE,a,1,b,2,-3,-60,6,2,0,\"[1,2]\"", -1);
    check("CSI,A,B,PROBE,a,1,b,2,3,-60,6,2,0,\"[1,2] \"", 1);
    assert(csi_validate("CSI,A,B,PROBE,a,1,b,2,3,-60,6,2,0,\"[1,2]\"", scratch, 4) == -1);

    used = (size_t)snprintf(full, sizeof full, "CSI,A,B,PROBE,a,1,b,2,3,-60,6,128,0,\"[");
    for (i = 0; i < 128; ++i)
        used += (size_t)snprintf(full + used, sizeof full - used, "%s%d", i ? "," : "", i - 64);
    (void)snprintf(full + used, sizeof full - used, "]\"");
    check(full, 1);
    memset(long_row, 'x', sizeof long_row);
    memcpy(long_row, "CSI,", 4);
    long_row[sizeof long_row - 1] = '\0';
    check(long_row, -1);

    assert(fabs(csi_elapsed(INT64_C(9000000001000000), INT64_C(9000000000000000), 10000000) - 0.1) < 1e-12);
    assert(csi_elapsed(100, 100, 10000000) == 0.0);
    puts("CSI parser and timestamp arithmetic checks passed.");
    return 0;
}
