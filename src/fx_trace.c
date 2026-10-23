/* SPDX-License-Identifier: GPL-2.0-or-later
 * Effect (SFX) instance trace (fx_trace.h), for Ebrietas' trail (f001251720.fxr) piling up rocks
 * at high frame rates.
 *
 * The effect library (Dantelion2 FFX) keeps each effect instance in a 0x98-byte block from
 * sub_284fc70, made only by sub_2853020 (system, id, ...), which the FX system's vtable entry
 * sub_28eb7f0 wraps in a handle. The instance holds its effect id at +0, a time scale at +8, its
 * tree of effect nodes at +0x78 (0x100-byte nodes from sub_284daa0: parent +0x60, next sibling
 * +0x68, first child +0x70), flags at +0x88 and the handles referring to it at +0x90 (linked at
 * handle +0x18). Every live instance is on one list (head at 0x54b0000, linked at +0x70).
 *
 * Stop (sub_2850780) sets flag 8, which the root nodes' state machine reads to end. Kill
 * (sub_284ff20) sets flag 4: the next update (sub_284ff30, with the frame time in xmm0) frees the
 * whole tree. Once an instance has no nodes left, the collector (sub_2850d30) releases it
 * (sub_284fd50) and frees it. Emitters add child effects by id inside an instance (sub_2853560,
 * no new instance), so an effect can live only as nodes of another; the node count stands in for
 * the particle count.
 *
 * A forever-emitting instance ends only when its external value key 0 ("terminate") is set to
 * 1.0 (sub_28505f0): the game's sfx holder (vtable +0x20 = sub_1f75680, handle at +0x80) does it
 * in its update (sub_1f73850) once its flag bit 0 (end requested, +0x10) is set.
 *
 * Nodes older than 5 s get a line with their lifecycle fields; their particle system (node +0xa0)
 * is watched by wrapping its class's vtable update (+0x18) and finished test (+0x38) in place:
 * the wrappers call the original first with the same arguments and return its result.
 *
 * The once-a-second report runs from whichever hook first sees the second end: the main-frame
 * tick is not there when another patch took that function's entry (Atlas lamp travel does).
 *
 * Only reads: the hooks run before the game's own instructions and change no register or memory.
 *
 * Addresses are image offsets (eboot virtual address; Binary Ninja's minus 0x400000). */
#include "fx_trace.h"
#include "engine_hooks.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

enum {
    CREATE_RETURN = 0x28eb813,    /* sub_28eb7f0, right after call sub_2853020: rax = instance */
    INSTANCE_STOP = 0x2850780,    /* or byte [rdi+0x88], 8 */
    INSTANCE_KILL = 0x284ff20,    /* or byte [rdi+0x88], 4 */
    INSTANCE_RELEASE = 0x284fd50, /* (instance), from the collector before the free */
    INSTANCE_UPDATE = 0x284ff30,  /* (instance, context, float dt) */
    NODE_CREATE = 0x284daa0,      /* (system, pos, matrix, def, instance, parent, ...) */
    CHILD_EFFECT = 0x2853560,     /* (system, effect id, def, ?, ?, parent node, ...) */
    INSTANCE_EXTERNALS = 0x28505f0, /* (instance, external value set): key 0 = 1.0 ends it */
    HOLDER_CREATE = 0x1f75680,    /* SprjFD4 sfx holder vtable +0x20; keeps the handle at +0x80 */
    LIVE_LIST = 0x54b0000,
};
static const BbCode create_return_code = {6, 0x08d07dc40feaa530ull};
static const BbCode stop_code = {7, 0x3c13c04b3a59ce50ull};
static const BbCode kill_code = {7, 0x3c13cc4b3a59e2b4ull};
static const BbCode release_code = {15, 0x112fb6ddc4b7e884ull};
static const BbCode update_code = {20, 0xc56331ff3adcd756ull};
static const BbCode node_create_code = {21, 0x8e1c12827986e080ull};
static const BbCode child_effect_code = {20, 0x60b885ecf8e57f6bull};
static const BbCode externals_code = {6, 0x82d75a35aabe38aaull};

enum { INST_ID = 0x00, INST_NEXT = 0x70, INST_ROOT = 0x78, INST_FLAGS = 0x88, INST_HANDLES = 0x90 };
/* Node: definition +0x50, particle system +0xa0 (body +0x20); FXR: root node at +0x30. */
enum { NODE_DEF = 0x50, NODE_PS = 0xa0, FXR_ROOT = 0x30, FXR_MAGIC = 0x00525846, FXR_SPAN = 0x40000,
       NODES_SCANNED = 512, NODE_LINES = 8, WATCHED = 64, WRAPPED = 8, PS_LINES = 4, PS_DUMP = 0x100,
       PS_POINTERS = 4, PS_POINTED = 0x100 };
static const float OLD_NODE = 5.0f;
enum { NODE_INSTANCE = 0x58, HANDLE_NEXT = 0x18, FLAG_KILL = 4, FLAG_STOP = 8 };
/* Holder (game side): state +0x0c, flags +0x10 (bit 0 = end requested), owner +0x18. Value set:
 * keys +0x08, values +0x10, count +0x18. */
enum { HOLDER_HANDLE = 0x80, HOLDER_STATE = 0x0c, HOLDER_FLAGS = 0x10, HOLDER_OWNER = 0x18, HOLDER_DUMP = 0x80,
       OWNER_DUMP = 0x80, HOLDERS_SHOWN = 3, SET_KEYS = 0x08, SET_VALUES = 0x10, SET_COUNT = 0x18, SET_SHOWN = 8,
       EXTERNALS_LOGGED = 4 };
/* Ebrietas' rock effect (1251721) and the trail it wraps. */
static const int32_t DEFAULT_IDS[] = {1251721, 1251720};
enum { MAX_IDS = 16, MAX_TRACKED = 1024, SHOWN = 24, TOP = 8, CHILD_SLOTS = 1024,
       EVENT_BUDGET = 40, DEPTH = 6, NODE_CAP = 100000, LIST_CAP = 200000, ALL_NODES_CAP = 400000,
       CODE_END = 0x4000000, IMAGE_END = 0x6000000, FFX_CODE = 0x2840000, FFX_CODE_END = 0x2b40000 };
static const uint64_t SECOND = 1000000;

/* Child effects spawned per id since the last report, with the id of the last instance holding one. */
typedef struct {
    int32_t id, parent_id;
    uint32_t n;
} ChildCount;
static ChildCount g_children[CHILD_SLOTS], g_creations[CHILD_SLOTS];  /* creations: parent_id unused */
static ChildCount *count_slot(ChildCount *table, int32_t id);

typedef struct {
    uint64_t instance, created_us;
    int32_t id;
    uint32_t nodes_made, nodes_seen;
    int stopped, killed, externals;
} Tracked;

static int32_t g_ids[MAX_IDS];
static int g_id_count, g_all;
static Tracked g_tracked[MAX_TRACKED];
static int g_count;
static atomic_uint g_untracked, g_events, g_suppressed, g_updates, g_frames;
static atomic_uint g_dt_last, g_dt_min, g_dt_max, g_dt_any;  /* float bits */
static atomic_ullong g_next_report, g_window;                /* microseconds */
static atomic_int g_ticking;
static atomic_flag g_lock = ATOMIC_FLAG_INIT;

static void lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) __builtin_ia32_pause();
}
static void unlock(void) { atomic_flag_clear_explicit(&g_lock, memory_order_release); }

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

/* printf into out[used..size), returning the new length. */
static size_t append(char *out, size_t size, size_t used, const char *format, ...)
    __attribute__((format(printf, 4, 5)));

static uint64_t read64(uint64_t address) {
    uint64_t v = 0;
    return bb_read(address, &v, 8) ? v : 0;
}

static float as_float(unsigned bits) {
    float f;
    memcpy(&f, &bits, 4);
    return f;
}
static unsigned float_bits(float f) {
    unsigned bits;
    memcpy(&bits, &f, 4);
    return bits;
}

/* The hooks see instances the game is using right then, so they read them directly. */
static int read_id(uint64_t instance, int32_t *id) {
    if (!instance) return 0;
    memcpy(id, (const void *)(uintptr_t)(instance + INST_ID), 4);
    return 1;
}

static int followed(int32_t id) {
    if (g_all) return 1;
    for (int i = 0; i < g_id_count; ++i)
        if (g_ids[i] == id) return 1;
    return 0;
}

void bb_fx_trace_select(const char *ids) {
    g_all = 0;
    g_id_count = 0;
    if (ids && !strcmp(ids, "all")) {
        g_all = 1;
    } else if (ids && *ids) {
        for (const char *p = ids; *p && g_id_count < MAX_IDS;) {
            char *end;
            const long id = strtol(p, &end, 10);
            if (end == p) break;
            g_ids[g_id_count++] = (int32_t)id;
            p = *end == ',' ? end + 1 : end;
        }
    }
    if (!g_all && !g_id_count)
        while (g_id_count < (int)(sizeof(DEFAULT_IDS) / sizeof(*DEFAULT_IDS)))
            g_ids[g_id_count] = DEFAULT_IDS[g_id_count], ++g_id_count;
    lock();
    g_count = 0;
    memset(g_children, 0, sizeof(g_children));
    memset(g_creations, 0, sizeof(g_creations));
    unlock();
    const uint64_t now = now_us();
    atomic_store(&g_window, now);
    atomic_store(&g_next_report, now + SECOND);
    atomic_store(&g_untracked, 0);
    atomic_store(&g_events, 0);
    atomic_store(&g_suppressed, 0);
    atomic_store(&g_updates, 0);
    atomic_store(&g_frames, 0);
    atomic_store(&g_dt_last, 0);
    atomic_store(&g_dt_any, 0);
}

static Tracked *find(uint64_t instance) {
    for (int i = 0; i < g_count; ++i)
        if (g_tracked[i].instance == instance) return &g_tracked[i];
    return NULL;
}

static int live_of(int32_t id) {
    int n = 0;
    for (int i = 0; i < g_count; ++i) n += g_tracked[i].id == id;
    return n;
}

static int collect_nodes(uint64_t instance, int cap, uint64_t *out, int out_size);

/* Lines beyond the budget of each wall-clock second are counted in the next report instead. */
static int may_log(void) {
    const uint64_t now = now_us();
    uint64_t start = atomic_load(&g_window);
    if (now - start >= SECOND && atomic_compare_exchange_strong(&g_window, &start, now))
        atomic_store(&g_events, 0);
    if (atomic_fetch_add(&g_events, 1) < EVENT_BUDGET) return 1;
    atomic_fetch_add(&g_suppressed, 1);
    return 0;
}

/* Run by whichever hook first sees the second over (one thread wins the exchange). */
static void maybe_report(void) {
    const uint64_t now = now_us();
    uint64_t due = atomic_load_explicit(&g_next_report, memory_order_relaxed);
    if (now < due || !atomic_compare_exchange_strong(&g_next_report, &due, now + SECOND)) return;
    bb_fx_trace_report((uint64_t)(uintptr_t)bb_guest(LIVE_LIST), now);
}

/* An address in the image as an image offset (code or data), else 0. */
static uint64_t image_offset(uint64_t address) {
    const uint64_t base = (uint64_t)(uintptr_t)bb_guest(0);
    if (!base) return address;
    return address > base && address - base < IMAGE_END ? address - base : 0;
}

static int in_image(uint64_t address) { return bb_guest(0) && image_offset(address); }

static uint64_t code_offset(uint64_t address) {
    const uint64_t base = (uint64_t)(uintptr_t)bb_guest(0);
    if (!base) return address;
    return address > base && address - base < CODE_END ? address - base : 0;
}

/* " from=a<b<c": the return address `first` (when known), then the frame pointer chain. */
static void format_from(char *out, size_t size, uint64_t first, uint64_t frame) {
    size_t used = (size_t)snprintf(out, size, " from=");
    int shown = 0;
    if (first && code_offset(first))
        used += (size_t)snprintf(out + used, size - used, "0x%llx", (unsigned long long)code_offset(first)),
            ++shown;
    for (int i = 0; i < DEPTH && frame && used < size; ++i) {
        const uint64_t ret = code_offset(read64(frame + 8));
        if (!ret) break;
        used += (size_t)snprintf(out + used, size - used, "%s0x%llx", shown++ ? "<" : "", (unsigned long long)ret);
        frame = read64(frame);
    }
    if (!shown) snprintf(out, size, " from=?");
}

void bb_fx_trace_created(uint64_t instance, uint64_t frame) {
    maybe_report();
    int32_t id;
    if (!read_id(instance, &id) || !followed(id)) return;
    lock();
    ChildCount *made = count_slot(g_creations, id);
    if (made) made->id = id, ++made->n;
    if (!find(instance)) {
        if (g_count < MAX_TRACKED)
            g_tracked[g_count++] = (Tracked){.instance = instance, .created_us = now_us(), .id = id};
        else
            atomic_fetch_add(&g_untracked, 1);
    }
    const int live = live_of(id);
    unlock();
    if (!may_log()) return;
    char from[160];
    format_from(from, sizeof(from), 0, frame);
    printf("FX: create %d inst=0x%llx live=%d%s\n", id, (unsigned long long)instance, live, from);
    fflush(stdout);
}

void bb_fx_trace_flagged(uint64_t instance, int kill, uint64_t rsp, uint64_t frame) {
    int32_t id;
    if (!read_id(instance, &id) || !followed(id)) return;
    lock();
    Tracked *t = find(instance);
    const uint64_t created = t ? t->created_us : 0;
    int again = 0;
    if (t) {
        int *flag = kill ? &t->killed : &t->stopped;
        again = (*flag)++ > 0;
    }
    const int live = live_of(id);
    unlock();
    if (!may_log()) return;
    char from[160], age[32] = "?";
    format_from(from, sizeof(from), rsp ? read64(rsp) : 0, frame);
    if (created) snprintf(age, sizeof(age), "%.2fs", (double)(now_us() - created) / 1e6);
    printf("FX: %s %d inst=0x%llx age=%s nodes=%d handles=%d live=%d%s%s\n", kill ? "kill" : "stop", id,
           (unsigned long long)instance, age, bb_fx_trace_nodes(instance, NODE_CAP),
           bb_fx_trace_handles(instance, NODE_CAP), live, again ? " again" : "", from);
    fflush(stdout);
}

void bb_fx_trace_externals(uint64_t instance, uint64_t set, uint64_t rsp, uint64_t frame) {
    int32_t id;
    if (!read_id(instance, &id) || !followed(id)) return;
    uint32_t count = 0, keys[SET_SHOWN], values[SET_SHOWN];
    uint64_t key_at = 0, value_at = 0;
    if (!set || !bb_read(set + SET_COUNT, &count, 4) || !bb_read(set + SET_KEYS, &key_at, 8) ||
        !bb_read(set + SET_VALUES, &value_at, 8))
        count = 0;
    const uint32_t shown = count < SET_SHOWN ? count : SET_SHOWN;
    if (shown && (!bb_read(key_at, keys, 4 * shown) || !bb_read(value_at, values, 4 * shown))) count = 0;
    int terminate = 0;
    for (uint32_t i = 0; i < shown && count; ++i) terminate |= keys[i] == 0 && as_float(values[i]) == 1.0f;
    lock();
    Tracked *t = find(instance);
    const uint64_t created = t ? t->created_us : 0;
    const int seen = t ? t->externals++ : 0;
    unlock();
    if ((!terminate && (seen >= EXTERNALS_LOGGED || !count)) || !may_log()) return;
    char from[200], age[32] = "?", values_text[160] = "";
    format_from(from, sizeof(from), rsp ? read64(rsp) : 0, frame);
    if (created) snprintf(age, sizeof(age), "%.2fs", (double)(now_us() - created) / 1e6);
    size_t used = 0;
    for (uint32_t i = 0; i < shown && count; ++i)
        used = append(values_text, sizeof(values_text), used, "%s%u:%g", i ? "," : "", keys[i], as_float(values[i]));
    printf("FX: %s %d inst=0x%llx age=%s set=[%s]%s%s\n", terminate ? "terminate" : "externals", id,
           (unsigned long long)instance, age, values_text, count > SET_SHOWN ? ",..." : "", from);
    fflush(stdout);
}

void bb_fx_trace_destroyed(uint64_t instance) {
    int32_t id;
    if (!read_id(instance, &id) || !followed(id)) return;
    lock();
    Tracked *t = find(instance);
    const Tracked gone = t ? *t : (Tracked){0};
    if (t) *t = g_tracked[--g_count];
    const int live = live_of(id);
    unlock();
    if (!may_log()) return;
    char age[32] = "?";
    if (gone.created_us) snprintf(age, sizeof(age), "%.2fs", (double)(now_us() - gone.created_us) / 1e6);
    printf("FX: destroy %d inst=0x%llx age=%s stopped=%d killed=%d live=%d\n", id,
           (unsigned long long)instance, age, gone.stopped, gone.killed, live);
    fflush(stdout);
}


/* Keeps the smaller (want_min) or larger value of `slot` and `dt`; 0 bits means unset. */
static void keep_extreme(atomic_uint *slot, float dt, int want_min) {
    unsigned seen = atomic_load(slot);
    while (!seen || (want_min ? dt < as_float(seen) : dt > as_float(seen)))
        if (atomic_compare_exchange_weak(slot, &seen, float_bits(dt))) break;
}

void bb_fx_trace_updated(uint64_t instance, float dt) {
    maybe_report();
    atomic_store_explicit(&g_dt_any, float_bits(dt), memory_order_relaxed);
    int32_t id;
    if (!read_id(instance, &id) || !followed(id)) return;
    atomic_fetch_add_explicit(&g_updates, 1, memory_order_relaxed);
    atomic_store_explicit(&g_dt_last, float_bits(dt), memory_order_relaxed);
    keep_extreme(&g_dt_min, dt, 1);
    keep_extreme(&g_dt_max, dt, 0);
}

void bb_fx_trace_node(uint64_t instance) {
    int32_t id;
    if (!read_id(instance, &id) || !followed(id)) return;
    lock();
    Tracked *t = find(instance);
    if (t) ++t->nodes_made;
    unlock();
}

static ChildCount *count_slot(ChildCount *table, int32_t id) {
    for (unsigned i = (uint32_t)id * 2654435761u % CHILD_SLOTS, probes = 0; probes < CHILD_SLOTS;
         i = (i + 1) % CHILD_SLOTS, ++probes)
        if (!table[i].n || table[i].id == id) return &table[i];
    return NULL;
}

void bb_fx_trace_child(int32_t id, uint64_t parent_node) {
    uint64_t instance = 0;
    int32_t parent_id = 0;
    if (parent_node) memcpy(&instance, (const void *)(uintptr_t)(parent_node + NODE_INSTANCE), 8);
    if (!read_id(instance, &parent_id)) parent_id = 0;
    const int wanted = !g_all && followed(id);
    if (!g_all && !wanted && !followed(parent_id)) return;
    lock();
    ChildCount *c = count_slot(g_children, id);
    if (c) c->id = id, c->parent_id = parent_id, ++c->n;
    unlock();
    /* Under "all" these would use up the budget the instance events need. */
    if (!wanted || !may_log()) return;
    printf("FX: child %d in=%d inst=0x%llx parent=0x%llx\n", id, parent_id, (unsigned long long)instance,
           (unsigned long long)parent_node);
    fflush(stdout);
}

int bb_fx_trace_nodes(uint64_t instance, int cap) { return collect_nodes(instance, cap, NULL, 0); }

static int collect_nodes(uint64_t instance, int cap, uint64_t *out, int out_size) {
    int n = 0;
    uint64_t node = read64(instance + INST_ROOT);
    /* The library's own walk (sub_284f0e0): first child, else next sibling, else up. */
    while (node && n < cap) {
        uint64_t links[4];  /* instance, parent, sibling, child */
        if (!bb_read(node + NODE_INSTANCE, links, sizeof(links)) || links[0] != instance) break;
        if (out && n < out_size) out[n] = node;
        ++n;
        if (links[3]) {
            node = links[3];
            continue;
        }
        while (!links[2] && links[1] && bb_read(links[1] + NODE_INSTANCE, links, sizeof(links))) {}
        node = links[2];
    }
    return n;
}

int bb_fx_trace_handles(uint64_t instance, int cap) {
    int n = 0;
    for (uint64_t h = read64(instance + INST_HANDLES); h && n < cap; h = read64(h + HANDLE_NEXT)) ++n;
    return n;
}

typedef struct {
    int32_t id;
    int n, nodes;
} IdCount;

typedef struct {
    int total, nodes, biggest_nodes;
    int32_t biggest_id;
    uint64_t biggest;
} LiveWalk;

/* Counts the library's live instances per followed id (or per id, for "all", with their nodes). */
static LiveWalk walk_live(uint64_t list_head, IdCount *counts, int slots) {
    LiveWalk w = {0};
    for (uint64_t inst = read64(list_head); inst && w.total < LIST_CAP; inst = read64(inst + INST_NEXT)) {
        ++w.total;
        int32_t id;
        if (!bb_read(inst + INST_ID, &id, 4) || !followed(id)) continue;
        const int nodes = g_all && w.nodes < ALL_NODES_CAP ? bb_fx_trace_nodes(inst, NODE_CAP) : 0;
        w.nodes += nodes;
        if (nodes > w.biggest_nodes) w.biggest_nodes = nodes, w.biggest_id = id, w.biggest = inst;
        for (int i = (int)((uint32_t)id * 2654435761u % (uint32_t)slots), probes = 0; probes < slots;
             i = (i + 1) % slots, ++probes) {
            if (counts[i].n && counts[i].id != id) continue;
            counts[i].id = id;
            ++counts[i].n;
            counts[i].nodes += nodes;
            break;
        }
    }
    return w;
}

/* " name=[id:value,...]" for the TOP ids by `by` (0 instances, 1 nodes); `counts` is reordered. */
static size_t append_top(char *out, size_t size, size_t used, const char *name, IdCount *counts, int slots,
                         int by) {
    used = append(out, size, used, " %s=[", name);
    for (int shown = 0; shown < TOP; ++shown) {
        int best = -1;
        for (int i = shown; i < slots; ++i) {
            const int value = by ? counts[i].nodes : counts[i].n;
            if (counts[i].n > 0 && value > 0 && (best < 0 || value > (by ? counts[best].nodes : counts[best].n)))
                best = i;
        }
        if (best < 0) break;
        const IdCount top = counts[best];
        counts[best] = counts[shown];
        counts[shown] = top;
        used = by ? append(out, size, used, "%s%d:%d/%d", shown ? "," : "", top.id, top.nodes, top.n)
                  : append(out, size, used, "%s%d:%d", shown ? "," : "", top.id, top.n);
    }
    return append(out, size, used, "]");
}

static int list_count(const IdCount *counts, int slots, int32_t id) {
    for (int i = 0; i < slots; ++i)
        if (counts[i].n && counts[i].id == id) return counts[i].n;
    return 0;
}

static int by_age(const void *a, const void *b) {
    const uint64_t x = ((const Tracked *)a)->created_us, y = ((const Tracked *)b)->created_us;
    return x < y ? -1 : x > y;
}

static size_t append(char *out, size_t size, size_t used, const char *format, ...) {
    if (used >= size) return used;
    va_list args;
    va_start(args, format);
    const int n = vsnprintf(out + used, size - used, format, args);
    va_end(args);
    return n < 0 ? used : used + (size_t)n;
}

/* One "name=[a,b,...]" list over the instances shown. */
/* The sfx holder keeping a handle to `instance`, recognised by its vtable's create method. */
static uint64_t holder_of(uint64_t instance) {
    const uint64_t create = (uint64_t)(uintptr_t)bb_guest(0) + HOLDER_CREATE;
    int n = 0;
    for (uint64_t h = read64(instance + INST_HANDLES); h && n < NODE_CAP; h = read64(h + HANDLE_NEXT), ++n) {
        const uint64_t holder = h - HOLDER_HANDLE, vtable = read64(holder);
        if (vtable && read64(vtable + 0x20) == create) return holder;
    }
    return 0;
}

enum { NODES, NEW, HANDLES, STATE, HOLDER, AGE };
static size_t append_list(char *out, size_t size, size_t used, int what, const Tracked *t, int n,
                          const uint32_t *made, uint64_t now) {
    static const char *const names[] = {"nodes", "new", "handles", "state", "hflags", "age"};
    used = append(out, size, used, " %s=[", names[what]);
    for (int i = 0; i < n; ++i) {
        const char *sep = i ? "," : "";
        uint32_t flags = 0;
        switch (what) {
        case NODES: used = append(out, size, used, "%s%d", sep, bb_fx_trace_nodes(t[i].instance, NODE_CAP)); break;
        case NEW: used = append(out, size, used, "%s%u", sep, made[i]); break;
        case HANDLES:
            used = append(out, size, used, "%s%d", sep, bb_fx_trace_handles(t[i].instance, NODE_CAP));
            break;
        case STATE: {
            const char *state = !bb_read(t[i].instance + INST_FLAGS, &flags, 4) ? "?"
                                : flags & FLAG_KILL ? "kill"
                                : flags & FLAG_STOP ? "stop"
                                : "run";
            used = append(out, size, used, "%s%s", sep, state);
            break;
        }
        case HOLDER: {
            const uint64_t holder = holder_of(t[i].instance);
            used = holder && bb_read(holder + HOLDER_FLAGS, &flags, 4)
                       ? append(out, size, used, "%s0x%x", sep, flags & 0xff)
                       : append(out, size, used, "%s-", sep);
            break;
        }
        case AGE: used = append(out, size, used, "%s%.1f", sep, (double)(now - t[i].created_us) / 1e6); break;
        }
    }
    return append(out, size, used, "]");
}

/* Particle systems ([node+0xa0]) of old nodes of followed instances, as the report found them,
 * and the vtables whose update (+0x18) and finished test (+0x38) are wrapped to watch them. */
typedef struct {
    atomic_ullong ps, instance, node, logged_us;
    atomic_uint updates, done;  /* done: last finished-test result + 1, 0 = not seen */
} Watched;
typedef struct {
    atomic_ullong vtable, update, finished;
} Wrapped;
typedef uint64_t (BB_GUEST_ABI *SlotFn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static Watched g_watched[WATCHED];
static Wrapped g_wrapped[WRAPPED];
static atomic_ullong g_ps_window;
static atomic_uint g_ps_lines;

static Watched *watched(uint64_t ps) {
    for (int i = 0; i < WATCHED; ++i)
        if (atomic_load_explicit(&g_watched[i].ps, memory_order_relaxed) == ps) return &g_watched[i];
    return NULL;
}

static const Wrapped *wrapped(uint64_t vtable) {
    for (int i = 0; i < WRAPPED; ++i)
        if (atomic_load_explicit(&g_wrapped[i].vtable, memory_order_acquire) == vtable) return &g_wrapped[i];
    return NULL;
}

/* At most PS_LINES particle-system lines a wall-clock second. */
static int may_log_ps(void) {
    const uint64_t now = now_us();
    uint64_t start = atomic_load(&g_ps_window);
    if (now - start >= SECOND && atomic_compare_exchange_strong(&g_ps_window, &start, now))
        atomic_store(&g_ps_lines, 0);
    return atomic_fetch_add(&g_ps_lines, 1) < PS_LINES;
}

static size_t append_dwords(char *out, size_t size, size_t used, const char *name, uint64_t at, int bytes);

/* After a watched system's update: its first fields and what its heap pointers point at. */
static void print_ps(Watched *w, uint64_t ps) {
    static char text[4096];
    const uint64_t now = now_us();
    uint64_t last = atomic_load(&w->logged_us);
    if (now - last < SECOND || !atomic_compare_exchange_strong(&w->logged_us, &last, now) || !may_log_ps())
        return;
    int32_t id = 0;
    bb_read(atomic_load(&w->instance) + INST_ID, &id, 4);
    const unsigned done = atomic_load(&w->done);
    size_t used = append(text, sizeof(text), 0,
                         "FX: ps %d inst=0x%llx node=0x%llx ps=0x%llx vt=0x%llx updates=%u done=%s",
                         id, (unsigned long long)atomic_load(&w->instance), (unsigned long long)atomic_load(&w->node),
                         (unsigned long long)ps, (unsigned long long)image_offset(read64(ps)),
                         atomic_exchange(&w->updates, 0), done ? (done > 1 ? "1" : "0") : "?");
    used = append_dwords(text, sizeof(text), used, "o", ps, PS_DUMP);
    uint64_t fields[PS_DUMP / 8];
    int pointers = 0;
    if (bb_read(ps, fields, sizeof(fields)))
        for (int i = 1; i < PS_DUMP / 8 && pointers < PS_POINTERS; ++i) {
            uint64_t probe;
            if (!fields[i] || in_image(fields[i]) || (fields[i] & 7) || !bb_read(fields[i], &probe, 8)) continue;
            char name[16];
            snprintf(name, sizeof(name), "p%x", i * 8);
            used = append_dwords(text, sizeof(text), used, name, fields[i], PS_POINTED);
            ++pointers;
        }
    puts(text);
    fflush(stdout);
}

/* No calls before the original runs: a call could clobber vector arguments. */
static inline uint64_t vtable_of(uint64_t object) {
    uint64_t vtable;
    memcpy(&vtable, (const void *)(uintptr_t)object, 8);
    return vtable;
}

static BB_GUEST_ABI uint64_t on_ps_update(uint64_t ps, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    const Wrapped *slot = wrapped(vtable_of(ps));
    const uint64_t result = ((SlotFn)(uintptr_t)atomic_load(&slot->update))(ps, b, c, d, e, f);
    Watched *w = watched(ps);
    if (w) {
        atomic_fetch_add_explicit(&w->updates, 1, memory_order_relaxed);
        print_ps(w, ps);
    }
    return result;
}

static BB_GUEST_ABI uint64_t on_ps_finished(uint64_t ps, uint64_t b, uint64_t c, uint64_t d, uint64_t e,
                                            uint64_t f) {
    const Wrapped *slot = wrapped(vtable_of(ps));
    const uint64_t result = ((SlotFn)(uintptr_t)atomic_load(&slot->finished))(ps, b, c, d, e, f);
    Watched *w = watched(ps);
    if (w) atomic_store_explicit(&w->done, (result & 0xff) ? 2u : 1u, memory_order_relaxed);
    return result;
}

static int in_ffx_code(uint64_t address) {
    const uint64_t base = (uint64_t)(uintptr_t)bb_guest(0);
    if (!base) return address != 0;  /* tests: no image */
    return address >= base + FFX_CODE && address < base + FFX_CODE_END;
}

static void put_slot(uint64_t slot, uint64_t value) {
#ifdef _WIN32
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)slot, 8, PAGE_READWRITE, &old)) return;
    atomic_store((atomic_ullong *)(uintptr_t)slot, value);
    VirtualProtect((void *)(uintptr_t)slot, 8, old, &old);
#else
    atomic_store((atomic_ullong *)(uintptr_t)slot, value);
#endif
}

/* Wraps a particle-system vtable's update and finished test, once; only when both are library code.
 * The wrappers call the original first with every integer argument and hand back its result. */
static void wrap_vtable(uint64_t vtable) {
    if (!vtable || wrapped(vtable)) return;
    const uint64_t update = read64(vtable + 0x18), finished = read64(vtable + 0x38);
    if (!in_ffx_code(update) || !in_ffx_code(finished)) return;
    for (int i = 0; i < WRAPPED; ++i) {
        if (atomic_load(&g_wrapped[i].vtable)) continue;
        atomic_store(&g_wrapped[i].update, update);
        atomic_store(&g_wrapped[i].finished, finished);
        atomic_store_explicit(&g_wrapped[i].vtable, vtable, memory_order_release);
        put_slot(vtable + 0x18, (uint64_t)(uintptr_t)on_ps_update);
        put_slot(vtable + 0x38, (uint64_t)(uintptr_t)on_ps_finished);
        printf("FX: ps hook vt=0x%llx update=0x%llx finished=0x%llx\n", (unsigned long long)image_offset(vtable),
               (unsigned long long)image_offset(update), (unsigned long long)image_offset(finished));
        return;
    }
}

static void watch(uint64_t ps, uint64_t instance, uint64_t node) {
    if (watched(ps)) return;
    for (int i = 0; i < WATCHED; ++i) {
        uint64_t old = atomic_load(&g_watched[i].ps);
        if (old) {
            /* Reuse the entry of a system whose node no longer holds it. */
            if (read64(atomic_load(&g_watched[i].node) + NODE_PS) == old) continue;
        }
        atomic_store(&g_watched[i].ps, 0);
        atomic_store(&g_watched[i].instance, instance);
        atomic_store(&g_watched[i].node, node);
        atomic_store(&g_watched[i].logged_us, 0);
        atomic_store(&g_watched[i].updates, 0);
        atomic_store(&g_watched[i].done, 0);
        atomic_store_explicit(&g_watched[i].ps, ps, memory_order_release);
        return;
    }
}

/* Pointers in the node's first 0x100 bytes that land in the instance's FXR (the root node's
 * definition sits at file offset 0x30), as file offsets. */
static size_t append_fxr(char *out, size_t size, size_t used, uint64_t instance, uint64_t node) {
    const uint64_t root_def = read64(read64(instance + INST_ROOT) + NODE_DEF), base = root_def - FXR_ROOT;
    uint32_t magic = 0;
    if (!root_def || !bb_read(base, &magic, 4) || magic != FXR_MAGIC) return append(out, size, used, " fxr=?");
    uint64_t fields[0x100 / 8];
    if (!bb_read(node, fields, sizeof(fields))) return append(out, size, used, " fxr=?");
    used = append(out, size, used, " fxr=[");
    int shown = 0;
    for (int i = 0; i < 0x100 / 8; ++i)
        if (fields[i] >= base && fields[i] - base < FXR_SPAN)
            used = append(out, size, used, "%s+%x:0x%llx", shown++ ? "," : "", i * 8,
                          (unsigned long long)(fields[i] - base));
    return append(out, size, used, "]");
}

/* Old nodes of a followed instance: their lifecycle fields, and their particle systems watched. */
static int print_old_nodes(int32_t id, uint64_t instance, int budget) {
    static uint64_t nodes[NODES_SCANNED];
    static char text[1024];
    const int n = collect_nodes(instance, NODES_SCANNED, nodes, NODES_SCANNED);
    int printed = 0;
    for (int i = 0; i < n && i < NODES_SCANNED && printed < budget; ++i) {
        unsigned char b[0x100];
        if (!bb_read(nodes[i], b, sizeof(b))) continue;
        float age, dur, e0, ctrl_time = 0;
        uint32_t flags, phase, d8, ctrl_state = 0;
        uint64_t ctrl, parent, ps;
        memcpy(&age, b + 0x7c, 4), memcpy(&dur, b + 0xf0, 4), memcpy(&e0, b + 0xe0, 4);
        memcpy(&flags, b + 0x78, 4), memcpy(&phase, b + 0xdc, 4);
        memcpy(&ctrl, b + 0xc8, 8), memcpy(&d8, b + 0xd8, 4);
        memcpy(&parent, b + 0x60, 8), memcpy(&ps, b + NODE_PS, 8);
        if (!(age > OLD_NODE)) continue;
        if (ctrl) bb_read(ctrl + 4, &ctrl_state, 4), bb_read(ctrl + 8, &ctrl_time, 4);
        const uint64_t ps_vt = read64(ps);
        Watched *w = ps ? watched(ps) : NULL;
        const unsigned done = w ? atomic_load(&w->done) : 0;
        size_t used = append(text, sizeof(text), 0,
                             "FX: node %d inst=0x%llx node=0x%llx age=%.2f phase=%u dur=%g flags=0x%x ctrl_state=%u "
                             "ctrl_time=%g d8=%u e0=%g parent=0x%llx ps=0x%llx ps_vt=0x%llx ps_update=0x%llx "
                             "ps_finished=0x%llx ps_done=%s", id, (unsigned long long)instance,
                             (unsigned long long)nodes[i], age, phase, dur, flags, ctrl_state, ctrl_time,
                             d8, e0, (unsigned long long)parent, (unsigned long long)ps,
                             (unsigned long long)image_offset(ps_vt),
                             (unsigned long long)image_offset(read64(ps_vt + 0x18)),
                             (unsigned long long)image_offset(read64(ps_vt + 0x38)),
                             done ? (done > 1 ? "1" : "0") : "?");
        append_fxr(text, sizeof(text), used, instance, nodes[i]);
        puts(text);
        ++printed;
        if (ps && ps_vt) {
            watch(ps, instance, nodes[i]);
            wrap_vtable(ps_vt);
        }
    }
    return printed;
}

/* " name=[a,b,...]": `bytes` of dwords at `at`, in hex. */
static size_t append_dwords(char *out, size_t size, size_t used, const char *name, uint64_t at, int bytes) {
    uint32_t words[64];
    used = append(out, size, used, " %s=[", name);
    if (at && bytes <= (int)sizeof(words) && bb_read(at, words, (size_t)bytes))
        for (int i = 0; i < bytes / 4; ++i) used = append(out, size, used, "%s%x", i ? "," : "", words[i]);
    return append(out, size, used, "]");
}

/* The holder of a followed instance, its owner and their first fields (to spot an end timer). */
static void print_holder(int32_t id, uint64_t instance) {
    static char text[2048];
    const uint64_t holder = holder_of(instance);
    if (!holder) {
        printf("FX: holder %d inst=0x%llx holder=none\n", id, (unsigned long long)instance);
        return;
    }
    uint32_t state = 0, flags = 0;
    bb_read(holder + HOLDER_STATE, &state, 4);
    bb_read(holder + HOLDER_FLAGS, &flags, 4);
    const uint64_t owner = read64(holder + HOLDER_OWNER);
    size_t used = append(text, sizeof(text), 0,
                         "FX: holder %d inst=0x%llx holder=0x%llx vt=0x%llx state=%u flags=0x%x owner=0x%llx "
                         "owner_vt=0x%llx", id, (unsigned long long)instance, (unsigned long long)holder,
                         (unsigned long long)image_offset(read64(holder)), state, flags & 0xff,
                         (unsigned long long)owner, (unsigned long long)image_offset(read64(owner)));
    used = append_dwords(text, sizeof(text), used, "h", holder, HOLDER_DUMP);
    append_dwords(text, sizeof(text), used, "o", owner, OWNER_DUMP);
    puts(text);
}

void bb_fx_trace_report(uint64_t list_head, uint64_t now) {
    static Tracked snap[MAX_TRACKED];
    static uint32_t made[MAX_TRACKED];
    static IdCount counts[2048];
    static ChildCount children[CHILD_SLOTS], creations[CHILD_SLOTS];
    enum { SLOTS = sizeof(counts) / sizeof(*counts) };
    memset(counts, 0, sizeof(counts));
    const LiveWalk walk = walk_live(list_head, counts, SLOTS);

    lock();
    const int n = g_count;
    for (int i = 0; i < n; ++i) {
        snap[i] = g_tracked[i];
        g_tracked[i].nodes_seen = g_tracked[i].nodes_made;
    }
    memcpy(children, g_children, sizeof(children));
    memset(g_children, 0, sizeof(g_children));
    memcpy(creations, g_creations, sizeof(creations));
    memset(g_creations, 0, sizeof(g_creations));
    unlock();
    qsort(snap, (size_t)n, sizeof(*snap), by_age);
    for (int i = 0; i < n; ++i) made[i] = snap[i].nodes_made - snap[i].nodes_seen;

    const unsigned min = atomic_exchange(&g_dt_min, 0), max = atomic_exchange(&g_dt_max, 0);
    const unsigned frames = atomic_exchange(&g_frames, 0);
    char frames_text[16] = "?", tail[288];
    if (atomic_load(&g_ticking)) snprintf(frames_text, sizeof(frames_text), "%u", frames);
    snprintf(tail, sizeof(tail),
             " dt=%.4f dtmin=%.4f dtmax=%.4f dt_any=%.4f updates=%u frames=%s total=%d untracked=%u suppressed=%u",
             as_float(atomic_load(&g_dt_last)), as_float(min), as_float(max), as_float(atomic_load(&g_dt_any)),
             atomic_exchange(&g_updates, 0), frames_text, walk.total, atomic_load(&g_untracked),
             atomic_exchange(&g_suppressed, 0));

    static char line[4096];
    int node_lines = 0;
    if (g_all) {
        size_t used = append(line, sizeof(line), 0, "FX: second all live=%d nodes=%d", n, walk.nodes);
        used = append_top(line, sizeof(line), used, "top_live", counts, SLOTS, 0);
        used = append_top(line, sizeof(line), used, "top_nodes", counts, SLOTS, 1);
        used = append(line, sizeof(line), used, " biggest=%d:%d@0x%llx", walk.biggest_id, walk.biggest_nodes,
                      (unsigned long long)walk.biggest);
        used = append(line, sizeof(line), used, " top_created=[");
        for (int shown = 0; shown < TOP; ++shown) {
            int best = -1;
            for (int i = 0; i < CHILD_SLOTS; ++i)
                if (creations[i].n && (best < 0 || creations[i].n > creations[best].n)) best = i;
            if (best < 0) break;
            used = append(line, sizeof(line), used, "%s%d:%u", shown ? "," : "", creations[best].id,
                          creations[best].n);
            creations[best].n = 0;
        }
        used = append(line, sizeof(line), used, "] top_child=[");
        for (int shown = 0; shown < TOP; ++shown) {
            int best = -1;
            for (int i = 0; i < CHILD_SLOTS; ++i)
                if (children[i].n && (best < 0 || children[i].n > children[best].n)) best = i;
            if (best < 0) break;
            used = append(line, sizeof(line), used, "%s%d:%u<%d", shown ? "," : "", children[best].id,
                          children[best].n, children[best].parent_id);
            children[best].n = 0;
        }
        append(line, sizeof(line), used, "]%s", tail);
        puts(line);
    }
    for (int k = 0; !g_all && k < g_id_count; ++k) {
        const int32_t id = g_ids[k];
        Tracked mine[SHOWN];
        uint32_t mine_made[SHOWN];
        int count = 0;
        for (int i = 0; i < n; ++i) {
            if (snap[i].id != id) continue;
            if (count < SHOWN) mine[count] = snap[i], mine_made[count] = made[i];
            ++count;
        }
        uint32_t as_child = 0, spawned_in = 0, created = 0;
        const ChildCount *c = count_slot(creations, id);
        if (c) created = c->n;
        int32_t child_in = 0;
        for (int i = 0; i < CHILD_SLOTS; ++i) {
            if (!children[i].n) continue;
            if (children[i].id == id) as_child += children[i].n, child_in = children[i].parent_id;
            if (children[i].parent_id == id) spawned_in += children[i].n;
        }
        size_t used = append(line, sizeof(line), 0, "FX: second id=%d live=%d list=%d created=%u", id, count,
                             list_count(counts, SLOTS, id), created);
        const int shown = count < SHOWN ? count : SHOWN;
        for (int what = NODES; what <= AGE; ++what)
            used = append_list(line, sizeof(line), used, what, mine, shown, mine_made, now);
        if (count > SHOWN) used = append(line, sizeof(line), used, " more=%d", count - SHOWN);
        used = append(line, sizeof(line), used, " child=%u child_in=%d children=%u", as_child, child_in,
                      spawned_in);
        append(line, sizeof(line), used, "%s", tail);
        puts(line);
        for (int i = 0; i < shown && i < HOLDERS_SHOWN; ++i) print_holder(id, mine[i].instance);
        for (int i = 0; i < shown && node_lines < NODE_LINES; ++i)
            node_lines += print_old_nodes(id, mine[i].instance, NODE_LINES - node_lines);
    }
    fflush(stdout);
}

static BB_GUEST_ABI void on_create_return(BbRegs *regs) { bb_fx_trace_created(regs->rax, regs->rbp); }
static BB_GUEST_ABI void on_stop(BbRegs *regs) {
    bb_fx_trace_flagged(regs->rdi, 0, bb_regs_rsp(regs), regs->rbp);
}
static BB_GUEST_ABI void on_kill(BbRegs *regs) {
    bb_fx_trace_flagged(regs->rdi, 1, bb_regs_rsp(regs), regs->rbp);
}
static BB_GUEST_ABI void on_release(uint64_t instance) { bb_fx_trace_destroyed(instance); }
static BB_GUEST_ABI void on_update(uint64_t instance, uint64_t context, float dt) {
    (void)context;
    bb_fx_trace_updated(instance, dt);
}
static BB_GUEST_ABI void on_node(uint64_t system, uint64_t pos, uint64_t matrix, uint64_t def,
                                 uint64_t instance) {
    (void)system, (void)pos, (void)matrix, (void)def;
    bb_fx_trace_node(instance);
}

static BB_GUEST_ABI void on_externals(BbRegs *regs) {
    bb_fx_trace_externals(regs->rdi, regs->rsi, bb_regs_rsp(regs), regs->rbp);
}
static BB_GUEST_ABI void on_child(uint64_t system, int32_t id, uint64_t def, uint64_t a, uint64_t b,
                                  uint64_t parent) {
    (void)system, (void)def, (void)a, (void)b;
    bb_fx_trace_child(id, parent);
}

/* Counts frames when the main-frame hook is there; the report does not depend on it. */
static void tick(void) {
    atomic_store_explicit(&g_ticking, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_frames, 1, memory_order_relaxed);
    maybe_report();
}

int bb_fx_trace_install(void) {
    const char *env = getenv("BB_FX_TRACE");
    if (!env || strcmp(env, "1")) return 0;
    struct {
        uint64_t at;
        const BbCode *code;
    } const sites[] = {{CREATE_RETURN, &create_return_code}, {INSTANCE_STOP, &stop_code},
                       {INSTANCE_KILL, &kill_code},          {INSTANCE_RELEASE, &release_code},
                       {INSTANCE_UPDATE, &update_code},      {NODE_CREATE, &node_create_code},
                       {CHILD_EFFECT, &child_effect_code},   {INSTANCE_EXTERNALS, &externals_code}};
    /* All or nothing: a half-installed trace would read as instances never stopped. */
    for (size_t i = 0; i < sizeof(sites) / sizeof(*sites); ++i)
        if (!bb_code_is(bb_guest(sites[i].at), *sites[i].code)) {
            printf("FX trace: off - unexpected code at 0x%llx (not the 1.09 image?)\n",
                   (unsigned long long)sites[i].at);
            return 0;
        }
    bb_fx_trace_select(getenv("BB_FX_TRACE_ID"));
    const int hooks = !bb_hook_mid(CREATE_RETURN, create_return_code, on_create_return) +
                      !bb_hook_mid(INSTANCE_STOP, stop_code, on_stop) +
                      !bb_hook_mid(INSTANCE_KILL, kill_code, on_kill) +
                      !bb_hook_entry(INSTANCE_RELEASE, release_code, (void *)on_release) +
                      !bb_hook_entry(INSTANCE_UPDATE, update_code, (void *)on_update) +
                      !bb_hook_entry(NODE_CREATE, node_create_code, (void *)on_node) +
                      !bb_hook_entry(CHILD_EFFECT, child_effect_code, (void *)on_child) +
                      !bb_hook_mid(INSTANCE_EXTERNALS, externals_code, on_externals);
    if (hooks != (int)(sizeof(sites) / sizeof(*sites)))
        printf("FX trace: only %d of %d hooks went in (%s)\n", hooks, (int)(sizeof(sites) / sizeof(*sites)),
               bb_hooks_error());
    bb_on_main_frame(tick);
    printf("FX trace: on, %d hooks, following", hooks);
    if (g_all) printf(" all effect ids");
    for (int i = 0; i < g_id_count; ++i) printf(" %d", g_ids[i]);
    putchar('\n');
    fflush(stdout);
    return hooks;
}
