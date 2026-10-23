/* SPDX-License-Identifier: GPL-2.0-or-later
 * Diagnostic trace of the game's effect (SFX) instances (fx_trace.c): BB_FX_TRACE=1 logs their
 * creation, stop, kill and destruction and, once a second, the live ones with their effect
 * nodes. Read-only; off by default, and nothing is hooked when off. */
#ifndef BB_FX_TRACE_H
#define BB_FX_TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hooks the effect library when BB_FX_TRACE=1; returns hooks added. */
int bb_fx_trace_install(void);

/* The trace's bookkeeping, called by its hooks; exposed for tests/test_fx_trace.c. */

/* Effect ids to follow, from BB_FX_TRACE_ID: unset or empty for Ebrietas' trail (1251720),
 * "all", or a comma-separated list. */
void bb_fx_trace_select(const char *ids);
/* An instance the library just created; `frame` is the creating function's frame pointer. */
void bb_fx_trace_created(uint64_t instance, uint64_t frame);
/* Stop (kill = 0) or kill (kill = 1) requested at an instance-flag function's entry. */
void bb_fx_trace_flagged(uint64_t instance, int kill, uint64_t rsp, uint64_t frame);
/* An instance the library's collector is about to free. */
void bb_fx_trace_destroyed(uint64_t instance);
/* An instance's update with the frame time it receives. */
void bb_fx_trace_updated(uint64_t instance, float dt);
/* An effect node created under an instance (its root, or a spawned child). */
void bb_fx_trace_node(uint64_t instance);
/* The once-a-second lines; `list_head` holds the library's list of live instances. */
void bb_fx_trace_report(uint64_t list_head, uint64_t now_us);

/* Nodes in an instance's effect tree and handles still referring to it, up to `cap`. */
int bb_fx_trace_nodes(uint64_t instance, int cap);
int bb_fx_trace_handles(uint64_t instance, int cap);

#ifdef __cplusplus
}
#endif

#endif
