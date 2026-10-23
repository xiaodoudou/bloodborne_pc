/* SPDX-License-Identifier: GPL-2.0-or-later
 * fx_trace.c on hand-made effect instances: the node and handle walks follow the library's
 * layout, the event and once-a-second lines carry the ids, counts, flags, child effects and
 * callers, ids are selected from BB_FX_TRACE_ID's forms, and the event budget refills each
 * wall-clock second while the hooks themselves start the report. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#include "engine_hooks.h"
#include "fx_trace.h"

enum { TRAIL = 1251720, OTHER = 42 };

typedef struct {
    _Alignas(16) unsigned char b[0x100];
} Block;

static Block inst[4], node[8], handle[3], holder, owner;
static uint64_t holder_vtable[8];
/* A particle system with its vtable (update +0x18, finished test +0x38) and a pointed-to block. */
static Block ps, particles;
static uint64_t ps_vtable[8];
static _Alignas(16) unsigned char fxr[0x5000];
static int ps_updates;
typedef uint64_t (BB_GUEST_ABI *SlotFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static BB_GUEST_ABI uint64_t ps_update(uint64_t self, uint64_t context, uint64_t commands) {
    assert(self == (uint64_t)(uintptr_t)ps.b && context == 1 && commands == 2);
    ++ps_updates;
    return 77;
}
static BB_GUEST_ABI uint64_t ps_finished(uint64_t self) { return self == (uint64_t)(uintptr_t)ps.b; }
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
    /* inst[1] is kept by an sfx holder: vtable +0x20 is the holders' create method (image offset,
     * with no image here), handle at +0x80, state +0x0c, flags +0x10, owner +0x18. */
    holder_vtable[4] = 0x1f75680;
    put64(&holder, 0, (uint64_t)(uintptr_t)holder_vtable);
    put32(&holder, 0x0c, 1);
    put32(&holder, 0x10, 0x1);
    put64(&holder, 0x18, at(&owner));
    put32(&owner, 0x20, 0x7f);
    put64(&inst[1], 0x90, at(&holder) + 0x80);
    assert(bb_fx_trace_handles(at(&inst[1]), 100) == 1);
    /* node[1] is old (age 6 s, phase 7) with a particle system; the FXR's root node at +0x30 is
     * the root node's definition, node[1]'s is at file offset 0x4098. */
    memcpy(fxr, "FXR", 4);
    put64(&node[0], 0x50, (uint64_t)(uintptr_t)(fxr + 0x30));
    put64(&node[1], 0x50, (uint64_t)(uintptr_t)(fxr + 0x4098));
    const float six = 6.0f, one = 1.0f;
    memcpy(node[1].b + 0x7c, &six, 4);
    memcpy(node[1].b + 0xf0, &one, 4);
    put32(&node[1], 0xdc, 7);
    put64(&node[1], 0xa0, at(&ps));
    ps_vtable[3] = (uint64_t)(uintptr_t)ps_update;
    ps_vtable[7] = (uint64_t)(uintptr_t)ps_finished;
    put64(&ps, 0, (uint64_t)(uintptr_t)ps_vtable);
    put64(&ps, 8, at(&particles));
    put32(&particles, 4, 0x41200000);
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
    bb_fx_trace_child(TRAIL, at(&node[1]));  /* logged: a followed id */
    bb_fx_trace_child(OTHER, at(&node[0]));  /* counted: inside a followed instance */
    bb_fx_trace_child(OTHER, at(&node[5]));  /* neither */
    put32(&inst[0], 0x88, 3 | 8);  /* what the stop then sets */
    bb_fx_trace_flagged(at(&inst[0]), 0, (uint64_t)(uintptr_t)&stop_return, frame);
    bb_fx_trace_flagged(at(&inst[0]), 0, 0, 0);
    /* External values: key 0 = 1.0 is the end request; others are logged a few times. */
    uint32_t keys[2] = {0, 3}, values[2] = {0x3f800000, 0x3f000000}, other_key = 2;
    Block set, other_set;
    put64(&set, 0x08, (uint64_t)(uintptr_t)keys);
    put64(&set, 0x10, (uint64_t)(uintptr_t)values);
    put32(&set, 0x18, 2);
    put64(&other_set, 0x08, (uint64_t)(uintptr_t)&other_key);
    put64(&other_set, 0x10, (uint64_t)(uintptr_t)values);
    put32(&other_set, 0x18, 1);
    bb_fx_trace_externals(at(&inst[0]), at(&set), (uint64_t)(uintptr_t)&stop_return, frame);
    bb_fx_trace_externals(at(&inst[1]), at(&other_set), 0, 0);
    bb_fx_trace_externals(at(&inst[2]), at(&set), 0, 0);  /* not followed */
    bb_fx_trace_report(list, now_us() + 2000000);
    /* The report wrapped the system's vtable: the game's calls still reach the originals. */
    assert(ps_vtable[3] != (uint64_t)(uintptr_t)ps_update && ps_vtable[7] != (uint64_t)(uintptr_t)ps_finished);
    assert(((SlotFn)(uintptr_t)ps_vtable[3])(at(&ps), 1, 2, 0, 0, 0) == 77 && ps_updates == 1);
    assert(((SlotFn)(uintptr_t)ps_vtable[7])(at(&ps), 0, 0, 0, 0, 0) == 1);
    bb_fx_trace_destroyed(at(&inst[1]));
    bb_fx_trace_report(list, now_us());

    /* "all": every id; the top ids by instances and nodes from the library's list. */
    bb_fx_trace_select("all");
    bb_fx_trace_created(at(&inst[2]), 0);
    bb_fx_trace_child(OTHER, at(&node[5]));
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
    /* A second later the budget is back without a report, and the next hook call reports. */
    Sleep(1100);
    static Block late;
    make_instance(&late, TRAIL);
    bb_fx_trace_created(at(&late), 0);
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
    snprintf(want, sizeof(want), "FX: child 1251720 in=1251720 inst=0x%llx parent=0x%llx\n",
             (unsigned long long)at(&inst[0]), (unsigned long long)at(&node[1]));
    assert(strstr(text, want));
    assert(count_of(text, "FX: child ") == 1);
    snprintf(want, sizeof(want), "FX: terminate 1251720 inst=0x%llx age=0.00s set=[0:1,3:0.5] "
             "from=0x333<0x111<0x222\n", (unsigned long long)at(&inst[0]));
    assert(strstr(text, want));
    snprintf(want, sizeof(want), "FX: externals 1251720 inst=0x%llx age=0.00s set=[2:1] from=?\n",
             (unsigned long long)at(&inst[1]));
    assert(strstr(text, want));
    assert(count_of(text, "FX: terminate ") + count_of(text, "FX: externals ") == 2);
    snprintf(want, sizeof(want), "FX: holder 1251720 inst=0x%llx holder=none\n", (unsigned long long)at(&inst[0]));
    assert(strstr(text, want));
    snprintf(want, sizeof(want), "FX: holder 1251720 inst=0x%llx holder=0x%llx vt=0x%llx state=1 flags=0x1 "
             "owner=0x%llx owner_vt=0x0 h=[", (unsigned long long)at(&inst[1]), (unsigned long long)at(&holder),
             (unsigned long long)(uintptr_t)holder_vtable, (unsigned long long)at(&owner));
    assert(strstr(text, want));
    assert(strstr(text, "] o=[0,0,0,0,0,0,0,0,7f,0,"));
    assert(strstr(text, "FX: second id=1251721 live=0 "));  /* the default follows both ids */
    snprintf(want, sizeof(want), "FX: node 1251720 inst=0x%llx node=0x%llx age=6.00 phase=7 dur=1 flags=0x0 "
             "ctrl_state=0 ctrl_time=0 d8=0 e0=0 parent=0x%llx ps=0x%llx ps_vt=0x%llx ps_update=0x%llx "
             "ps_finished=0x%llx ps_done=? fxr=[+50:0x4098", (unsigned long long)at(&inst[0]),
             (unsigned long long)at(&node[1]), (unsigned long long)at(&node[0]), (unsigned long long)at(&ps),
             (unsigned long long)(uintptr_t)ps_vtable, (unsigned long long)(uintptr_t)ps_update,
             (unsigned long long)(uintptr_t)ps_finished);
    assert(strstr(text, want));
    snprintf(want, sizeof(want), "FX: ps hook vt=0x%llx update=0x%llx finished=0x%llx\n",
             (unsigned long long)(uintptr_t)ps_vtable, (unsigned long long)(uintptr_t)ps_update,
             (unsigned long long)(uintptr_t)ps_finished);
    assert(strstr(text, want) && count_of(text, "FX: ps hook ") == 1);
    snprintf(want, sizeof(want), "FX: ps 1251720 inst=0x%llx node=0x%llx ps=0x%llx vt=0x%llx updates=1 done=? o=[",
             (unsigned long long)at(&inst[0]), (unsigned long long)at(&node[1]), (unsigned long long)at(&ps),
             (unsigned long long)(uintptr_t)ps_vtable);
    assert(strstr(text, want));
    assert(strstr(text, "] p8=[0,41200000,0,"));
    assert(strstr(text, " ps_done=1 fxr=[+50:0x4098"));  /* the next report has the finished result */
    assert(strstr(text, "FX: second id=1251720 live=2 list=2 created=2 nodes=[5,0] new=[2,0] handles=[3,1] "
                        "state=[stop,run] hflags=[-,0x1] age=[2.0,2.0] child=1 child_in=1251720 children=2 dt=0.0111 "
                        "dtmin=0.0111 dtmax=0.0111 dt_any=0.5000 updates=3 frames=? total=3 untracked=0 "
                        "suppressed=0\n"));
    snprintf(want, sizeof(want), "FX: destroy 1251720 inst=0x%llx age=", (unsigned long long)at(&inst[1]));
    assert(strstr(text, want));
    assert(strstr(text, " stopped=0 killed=0 live=1\n"));
    assert(strstr(text, "FX: second id=1251720 live=1 list=2 created=0 nodes=[5] new=[0] handles=[3] "
                        "state=[stop] hflags=[-] "));
    assert(strstr(text, " child=0 child_in=0 children=0 "));
    assert(strstr(text, " updates=0 frames=? total=3 "));
    assert(count_of(text, "FX: create 42 ") == 2);
    snprintf(want, sizeof(want), "FX: second all live=1 nodes=5 top_live=[1251720:2,42:1] "
             "top_nodes=[1251720:5/2] biggest=1251720:5@0x%llx top_created=[42:1] top_child=[42:1<42] ",
             (unsigned long long)at(&inst[0]));
    assert(strstr(text, want));
    assert(strstr(text, "FX: second id=7 live=0 list=0 created=0 nodes=[] new=[] handles=[] state=[] hflags=[] "
                        "age=[] "));
    assert(strstr(text, "FX: second id=42 live=1 list=1 created=1 nodes=[0] "));
    assert(!strstr(text, "FX: create 1251720 inst=0x0"));
    assert(count_of(text, "FX: create 1251720 ") == 2 + 40 + 1);
    assert(strstr(text, "FX: second id=1251720 live=60 list=2 created=60 "));
    assert(strstr(text, " more=36 "));
    assert(strstr(text, " suppressed=20\n"));
    snprintf(want, sizeof(want), "FX: create 1251720 inst=0x%llx live=61 ", (unsigned long long)at(&late));
    assert(strstr(text, want));
    assert(strstr(text, "FX: second id=1251720 live=60 list=0 created=0 "));  /* from the hook, no list */
    remove(log);
    fprintf(stderr, "fx trace: ok\n");
    return 0;
}
