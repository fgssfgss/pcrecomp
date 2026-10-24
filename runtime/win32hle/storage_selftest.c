/*
 * storage_selftest.c - structured storage round trip: a compound file with a
 * small stream (the mini stream) and a big one (regular sectors) inside a
 * sub-storage, written, released, opened again and read back. Headless.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "win32hle.h"

const recomp_dispatch_entry_t recomp_dispatch_table[] = { { 0, 0 } };
const uint32_t recomp_dispatch_count = 0;

static int fails;
static void ok(const char *w, int c) { if (!c) { printf("FAIL %s\n", w); fails++; } }

static uint32_t call(const char *name, int n, const uint32_t *a) { return hle_call_guest(hle_resolve(name), n, a); }
static uint32_t vcall(uint32_t obj, int slot, int n, const uint32_t *args) {
    uint32_t a[8] = { obj };
    for (int i = 0; i < n; i++) a[1 + i] = args[i];
    return hle_call_guest(MEM32(MEM32(obj) + 4u * (uint32_t)slot), n + 1, a);
}
#define P(x) hle_va(x)                                /* everything handed over is guest memory */
static void wide(uint16_t *w, const char *s) { while ((*w++ = (uint8_t)*s++)) {} }

int main(void) {
    win32hle_register(win32hle_kernel32);
    win32hle_register(win32hle_ole32);
    win32hle_register(win32hle_storage);
    hle_set_drive('Z', "/");
    mach_enter();

    struct {
        uint16_t file[64], sub[16], small[16], big[16];
        uint8_t bigdata[10000], back[10000];
        char hello[18];
        uint32_t stg, s2, stm, n;
        uint32_t en, en2, fetched;
        uint8_t st[3 * 72];                           /* STATSTG[3] */
    } *g = hle_alloc(sizeof *g);
    wide(g->file, "Z:\\tmp\\win32hle_storage.sav");
    wide(g->sub, "Scenario"), wide(g->small, "Header"), wide(g->big, "Data");
    for (int i = 0; i < (int)sizeof g->bigdata; i++) g->bigdata[i] = (uint8_t)(i * 7);
    strcpy(g->hello, "Tiberian Sun save");

    uint32_t a[6] = { P(g->file), 0x1012u /* CREATE | READWRITE | SHARE_EXCLUSIVE */, 0, P(&g->stg) };
    ok("StgCreateDocfile", call("StgCreateDocfile", 4, a) == 0 && g->stg);
    uint32_t cs[5] = { P(g->sub), 0x12u, 0, 0, P(&g->s2) };
    ok("CreateStorage", vcall(g->stg, 5, 5, cs) == 0 && g->s2);
    uint32_t c1[5] = { P(g->small), 0x12u, 0, 0, P(&g->stm) };
    ok("CreateStream small", vcall(g->s2, 3, 5, c1) == 0 && g->stm);
    uint32_t w1[3] = { P(g->hello), sizeof g->hello, P(&g->n) };
    ok("Write small", vcall(g->stm, 4, 3, w1) == 0 && g->n == sizeof g->hello);
    vcall(g->stm, 2, 0, NULL);
    uint32_t c2[5] = { P(g->big), 0x12u, 0, 0, P(&g->stm) };
    ok("CreateStream big", vcall(g->s2, 3, 5, c2) == 0 && g->stm);
    uint32_t w2[3] = { P(g->bigdata), sizeof g->bigdata, P(&g->n) };
    ok("Write big", vcall(g->stm, 4, 3, w2) == 0 && g->n == sizeof g->bigdata);
    vcall(g->stm, 2, 0, NULL);
    vcall(g->s2, 2, 0, NULL);
    vcall(g->stg, 2, 0, NULL);                           /* the last release writes the file */

    uint32_t o[6] = { P(g->file), 0, 0x10u /* READ | SHARE_EXCLUSIVE */, 0, 0, P(&g->stg) };
    ok("StgOpenStorage", call("StgOpenStorage", 6, o) == 0 && g->stg);
    uint32_t os[6] = { P(g->sub), 0, 0x10u, 0, 0, P(&g->s2) };
    ok("OpenStorage", vcall(g->stg, 6, 6, os) == 0 && g->s2);
    uint32_t r1[5] = { P(g->small), 0, 0x10u, 0, P(&g->stm) };
    ok("OpenStream small", vcall(g->s2, 4, 5, r1) == 0 && g->stm);
    uint32_t rd[3] = { P(g->back), sizeof g->back, P(&g->n) };
    vcall(g->stm, 3, 3, rd);
    ok("small read back", g->n == sizeof g->hello && !memcmp(g->back, g->hello, sizeof g->hello));
    vcall(g->stm, 2, 0, NULL);
    uint32_t r2[5] = { P(g->big), 0, 0x10u, 0, P(&g->stm) };
    ok("OpenStream big", vcall(g->s2, 4, 5, r2) == 0 && g->stm);
    vcall(g->stm, 3, 3, rd);
    ok("big read back", g->n == sizeof g->bigdata && !memcmp(g->back, g->bigdata, sizeof g->bigdata));

    /* EnumElements over the sub-storage: both streams, in the compound file's
     * order (shorter name first), then Next past the end, Reset, Skip, Clone. */
    uint32_t ee[4] = { 0, 0, 0, P(&g->en) };
    ok("EnumElements", vcall(g->s2, 11, 4, ee) == 0 && g->en);
    uint32_t nx[3] = { 3, P(g->st), P(&g->fetched) };
    uint32_t hr = vcall(g->en, 3, 3, nx);
    const uint16_t *n0 = hle_ptr(*(uint32_t *)g->st), *n1 = hle_ptr(*(uint32_t *)(g->st + 72));
    ok("Next: both, S_FALSE for the third", hr == 1 && g->fetched == 2);
    ok("Next: Data then Header", n0 && n1 && n0[0] == 'D' && n0[4] == 0 && n1[0] == 'H' && n1[6] == 0);
    ok("Next: stream type and size", *(uint32_t *)(g->st + 4) == 2 && *(uint32_t *)(g->st + 8) == sizeof g->bigdata &&
       *(uint32_t *)(g->st + 72 + 8) == sizeof g->hello);
    uint32_t one[3] = { 1, P(g->st), P(&g->fetched) };
    ok("Next at the end: none", vcall(g->en, 3, 3, one) == 1 && g->fetched == 0);
    ok("Reset", vcall(g->en, 5, 0, NULL) == 0);
    uint32_t sk[1] = { 1 };
    ok("Skip one", vcall(g->en, 4, 1, sk) == 0);
    n0 = NULL;
    ok("Next after Skip: Header", vcall(g->en, 3, 3, one) == 0 && g->fetched == 1 &&
       (n0 = hle_ptr(*(uint32_t *)g->st)) && n0[0] == 'H');
    uint32_t cl[1] = { P(&g->en2) };
    ok("Clone at the end: nothing left", vcall(g->en, 6, 1, cl) == 0 && g->en2 &&
       vcall(g->en2, 3, 3, one) == 1 && g->fetched == 0);
    vcall(g->en2, 2, 0, NULL);
    vcall(g->en, 2, 0, NULL);

    vcall(g->stm, 2, 0, NULL);
    vcall(g->s2, 2, 0, NULL);
    vcall(g->stg, 2, 0, NULL);

    mach_leave();
    if (fails == 0) printf("storage_selftest: all checks passed\n");
    return fails != 0;
}
