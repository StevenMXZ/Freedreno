/* SPDX-License-Identifier: MIT */
#ifndef LOADER_DRI3_PRESENT_H
#define LOADER_DRI3_PRESENT_H
#include <stdbool.h>
#include <stdint.h>

/* Winlator does not implement DRI3 FenceFromFD. Match the Present request's
 * serial instead of a SyncFence XID, so a stale IdleNotify cannot release a
 * newer presentation using the same pixmap. Present serials are 32-bit.
 */
static inline bool
loader_dri3_idle_matches(uint32_t pixmap, uint64_t last_swap,
                        uint32_t event_pixmap, uint32_t serial,
                        uint32_t idle_fence)
{
   return pixmap == event_pixmap && (uint32_t)last_swap == serial && idle_fence == 0;
}
#endif
