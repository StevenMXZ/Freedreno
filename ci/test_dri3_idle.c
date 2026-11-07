/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include "../src/gallium/frontends/dri/loader_dri3_present.h"
int main(void) {
   assert(loader_dri3_idle_matches(42, 100, 42, 100, 0));
   assert(!loader_dri3_idle_matches(42, 101, 42, 100, 0)); /* old presentation */
   assert(!loader_dri3_idle_matches(42, 100, 43, 100, 0)); /* another pixmap */
   assert(!loader_dri3_idle_matches(42, 100, 42, 100, 9)); /* foreign fence */
   assert(loader_dri3_idle_matches(42, UINT64_C(0x100000001), 42, 1, 0));
   assert(!loader_dri3_idle_matches(42, UINT64_C(0x100000001), 42, UINT32_MAX, 0));
   puts("DRI3 IdleNotify: current/stale serials, pixmap and wraparound passed");
}
