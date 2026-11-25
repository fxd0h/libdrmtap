/*
 * test_abi.c - drmtap_abi() against DRMTAP_ABI_LEVEL, through the shared library.
 *
 * Linked against the version-scripted .so (not the static archive): a
 * drmtap_abi missing from libdrmtap.map fails the link of this test.
 */
#include <stdio.h>
#include <stdint.h>
#include "drmtap.h"
int main(void) {
    uint32_t cur = 0, old = 0;
    drmtap_abi(&cur, &old);
    printf("drmtap_abi: current %u oldest %u, header DRMTAP_ABI_LEVEL %d\n", cur, old,
           DRMTAP_ABI_LEVEL);
    int fail = 0;
    if (cur != (uint32_t)DRMTAP_ABI_LEVEL) {
        printf("FAIL: the library's current level is not the header's DRMTAP_ABI_LEVEL\n");
        fail = 1;
    }
    if (old < 1 || old > cur) {
        printf("FAIL: oldest level outside 1..current\n");
        fail = 1;
    }
    if (old != 1) {
        printf("FAIL: 0.6 keeps every 0.5 struct and call, so oldest must be 1\n");
        fail = 1;
    }
    drmtap_abi(NULL, NULL);   /* NULL is allowed for either */
    uint32_t only = 0;
    drmtap_abi(&only, NULL);
    if (only != cur) {
        printf("FAIL: current alone differs\n");
        fail = 1;
    }
    if (!fail) {
        printf("PASS\n");
    }
    return fail;
}
