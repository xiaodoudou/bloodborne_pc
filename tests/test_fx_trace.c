/* SPDX-License-Identifier: GPL-2.0-or-later
 * fx_trace.c on hand-made effect instances: the node and handle walks follow the library's
 * layout, the event and once-a-second lines carry the ids, counts, flags and callers, ids are
 * selected from BB_FX_TRACE_ID's forms, and the event lines stay within their budget. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fx_trace.h"

enum { TRAIL = 1251720, OTHER = 42 };

typedef struct {
    _Alignas(16) unsigned char b[0x100];
} Block;

static Block inst[4], node[8], handle[3];
static uint64_t frames[3][2];  /* saved frame pointer, return address */
static uint64_t stop_return = 0x333;

static uint64_t at(Block *b) { return (uint64_t)(uintptr_t)b->b; }
static void put64(Block *b, int offset, uint64_t v) { memcpy(b->b + offset, &v, 8); }
static void put32(Block *b, int offset, uint32_t v) { memcpy(b->b + offset, &v, 4); }

/* id at +0, next live instance +0x70, first root node +0x78, flags +0x88, handles +0x90. */
static void make_instance(Block *b, int32_t id) {
    memset(b, 0, sizeof(*b));
    put32(b, 0, (uint32_t)id);
}
/* Node: instance +0x58, parent +0x60, next sibling +0x68, first child +0x70. */
static void make_node(Block *n, Block *owner, Block *parent, Block *sibling, Block *child) {
    put64(n, 0x58, at(owner));
    put64(n, 0x60, parent ? at(parent) : 0);
    put64(n, 0x68, sibling ? at(sibling) : 0);
    put64(n, 0x70, child ? at(child) : 0);
}

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "r");  /* text mode: stdout wrote CRLF on Windows */
    assert(f);
    static char text[1 << 16];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    text[n] = 0;
    fclose(f);
    return text;
}

static int count_of(const char *text, const char *needle) {
    int n = 0;
    for (const char *p = strstr(text, needle); p; p = strstr(p + 1, needle)) ++n;
    return n;
}

int main(void) {
    const char *log = "fx-trace-test.log";
    /* Forest under inst[0]: A(B(D), C), E; then a node of another instance ends the walk. */
    make_instance(&inst[0], TRAIL);
    make_instance(&inst[1], TRAIL);
    make_instance(&inst[2], OTHER);
    make_node(&node[0], &inst[0], NULL, &node[4], &node[1]);      /* A */
    make_node(&node[1], &inst[0], &node[0], &node[2], &node[3]);  /* B */
    make_node(&node[2], &inst[0], &node[0], NULL, NULL);          /* C */
    make_node(&node[3], &inst[0], &node[1], NULL, NULL);          /* D */
    make_node(&node[4], &inst[0], NULL, &node[5], NULL);          /* E */
    make_node(&node[5], &inst[2], NULL, NULL, NULL);              /* foreign */
    put64(&inst[0], 0x78, at(&node[0]));
    assert(bb_fx_trace_nodes(at(&inst[0]), 1000) == 5);
    assert(bb_fx_trace_nodes(at(&inst[0]), 3) == 3);
    assert(bb_fx_trace_nodes(at(&inst[1]), 1000) == 0);
    /* Handles: linked from +0x90 through handle +0x18. */
    put64(&inst[0], 0x90, at(&handle[0]));
    put64(&handle[0], 0x18, at(&handle[1]));
    put64(&handle[1], 0x18, at(&handle[2]));
    assert(bb_fx_trace_handles(at(&inst[0]), 100) == 3);
    assert(bb_fx_trace_handles(at(&inst[1]), 100) == 0);
    /* The library's live list: inst[0] -> inst[1] -> inst[2]. */
    put64(&inst[0], 0x70, at(&inst[1]));
    put64(&inst[1], 0x70, at(&inst[2]));
    uint64_t head = at(&inst[0]);
    const uint64_t list = (uint64_t)(uintptr_t)&head;
    /* The creator's frame chain: returns 0x111, then 0x222. */
    frames[0][0] = (uint64_t)(uintptr_t)frames[1];
    frames[0][1] = 0x111;
    frames[1][0] = 0;
    frames[1][1] = 0x222;
    const uint64_t frame = (uint64_t)(uintptr_t)frames[0];

    assert(freopen(log, "w", stdout));
    bb_fx_trace_select(NULL);
    bb_fx_trace_created(at(&inst[0]), frame);
    bb_fx_trace_created(at(&inst[1]), 0);
    bb_fx_trace_created(at(&inst[2]), frame);  /* not followed */
    bb_fx_trace_created(0, frame);             /* a failed creation */
    for (int i = 0; i < 3; ++i) bb_fx_trace_updated(at(&inst[0]), 0.0111f);
    bb_fx_trace_updated(at(&inst[2]), 0.5f);
    bb_fx_trace_node(at(&inst[0]));
    bb_fx_trace_node(at(&inst[0]));
    bb_fx_trace_node(at(&inst[2]));
    put32(&inst[0], 0x88, 3 | 8);  /* what the stop then sets */
    bb_fx_trace_flagged(at(&inst[0]), 0, (uint64_t)(uintptr_t)&stop_return, frame);
    bb_fx_trace_flagged(at(&inst[0]), 0, 0, 0);
    bb_fx_trace_report(list, now_us() + 2000000);
    bb_fx_trace_destroyed(at(&inst[1]));
    bb_fx_trace_report(list, now_us());

    /* "all": every id, the top live ids from the library's list. */
    bb_fx_trace_select("all");
    bb_fx_trace_created(at(&inst[2]), 0);
    bb_fx_trace_report(list, now_us());
    /* A list: only those ids. */
    bb_fx_trace_select("7,42");
    bb_fx_trace_created(at(&inst[0]), 0);
    bb_fx_trace_created(at(&inst[2]), 0);
    bb_fx_trace_report(list, now_us());
    /* Budget: 40 event lines a second, the rest counted. */
    bb_fx_trace_select("");
    static Block many[60];
    for (int i = 0; i < 60; ++i) {
        make_instance(&many[i], TRAIL);
        bb_fx_trace_created(at(&many[i]), 0);
    }
    bb_fx_trace_report(list, now_us());
    fflush(stdout);

    const char *text = slurp(log);
    char want[256];
    snprintf(want, sizeof(want), "FX: create 1251720 inst=0x%llx live=1 from=0x111<0x222\n",
             (unsigned long long)at(&inst[0]));
    assert(strstr(text, want));
    snprintf(want, sizeof(want), "FX: create 1251720 inst=0x%llx live=2 from=?\n",
             (unsigned long long)at(&inst[1]));
    assert(strstr(text, want));
    snprintf(want, sizeof(want), "FX: stop 1251720 inst=0x%llx age=", (unsigned long long)at(&inst[0]));
    assert(strstr(text, want));
    assert(strstr(text, " nodes=5 handles=3 live=2 from=0x333<0x111<0x222\n"));
    assert(strstr(text, " nodes=5 handles=3 live=2 again from=?\n"));
    assert(strstr(text, "FX: second id=1251720 live=2 list=2 nodes=[5,0] new=[2,0] handles=[3,0] "
                        "state=[stop,run] age=[2.0,2.0] dt=0.0111 dtmin=0.0111 dtmax=0.0111 dt_any=0.5000 "
                        "updates=3 frames=0 total=3 untracked=0 suppressed=0\n"));
    snprintf(want, sizeof(want), "FX: destroy 1251720 inst=0x%llx age=", (unsigned long long)at(&inst[1]));
    assert(strstr(text, want));
    assert(strstr(text, " stopped=0 killed=0 live=1\n"));
    assert(strstr(text, "FX: second id=1251720 live=1 list=2 nodes=[5] new=[0] handles=[3] state=[stop] "));
    assert(strstr(text, " updates=0 frames=0 total=3 "));
    assert(count_of(text, "FX: create 42 ") == 2);
    assert(strstr(text, "FX: second all live=1 top=[1251720:2,42:1] "));
    assert(strstr(text, "FX: second id=7 live=0 list=0 nodes=[] new=[] handles=[] state=[] age=[] "));
    assert(strstr(text, "FX: second id=42 live=1 list=1 nodes=[0] "));
    assert(!strstr(text, "FX: create 1251720 inst=0x0"));
    assert(count_of(text, "FX: create 1251720 ") == 2 + 40);
    assert(strstr(text, "FX: second id=1251720 live=60 list=2 "));
    assert(strstr(text, " more=36 "));
    assert(strstr(text, " suppressed=20\n"));
    remove(log);
    fprintf(stderr, "fx trace: ok\n");
    return 0;
}
