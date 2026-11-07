/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#define __user
#include "../src/freedreno/vulkan/msm_kgsl.h"
static uint64_t now;
static unsigned retired, waits, polls, last_timeout;
static int interrupt, poll_error, wait_error;
static int64_t os_time_get_nano(void) { return now; }
static int test_ioctl(int fd, unsigned long request, void *data) {
   (void)fd;
   if (request == IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID) {
      polls++;
      if(poll_error) { errno=poll_error;return -1; }
      ((struct kgsl_cmdstream_readtimestamp_ctxtid*)data)->timestamp=retired;
      return 0;
   }
   assert(request==IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID);
   struct kgsl_device_waittimestamp_ctxtid *w=data;
   assert(w->timeout!=0); waits++;last_timeout=w->timeout;
   if(interrupt) { now+=2000000;errno=interrupt;interrupt=0;return -1; }
   if(wait_error) { errno=wait_error;return -1; }
   return 0;
}
#define ioctl test_ioctl
#define kgsl_safe_ioctl test_ioctl
#include "../src/freedreno/drm/kgsl/kgsl_wait.h"
static void reset(void) { now=10000000;retired=10;waits=polls=last_timeout=0;interrupt=poll_error=wait_error=0; }
int main(void) {
 reset();assert(wait_timestamp_safe(1,1,10,now)==0);assert(polls==1 && waits==0);
 reset();assert(wait_timestamp_safe(1,1,11,now)==-1 && errno==ETIMEDOUT);assert(waits==0);
 reset();assert(wait_timestamp_safe(1,1,11,now+100)==-1);assert(waits==0);
 reset();poll_error=EIO;assert(wait_timestamp_safe(1,1,10,now)==-1 && errno==EIO);
 reset();retired=1;assert(wait_timestamp_safe(1,1,UINT_MAX,now)==0);
 reset();interrupt=EINTR;assert(wait_timestamp_safe(1,1,10,now+1000000)==0);assert(waits==1 && polls==1);
 reset();interrupt=EAGAIN;assert(wait_timestamp_safe(1,1,11,now+1000000)==-1);assert(waits==1 && polls==1);
 reset();assert(wait_timestamp_safe(1,1,11,now+3000000)==0 && last_timeout==3);
 reset();assert(wait_timestamp_safe(1,1,11,UINT64_MAX)==0 && last_timeout==UINT_MAX);
 reset();assert(wait_timestamp_safe(1,1,11,now+((uint64_t)INT_MAX+42)*1000000)==0 && last_timeout==INT_MAX);
 reset();wait_error=EIO;assert(wait_timestamp_safe(1,1,11,now+3000000)==-1 && errno==EIO);
 puts("KGSL deadlines: zero/sub-ms polls, wraparound, retries, errors and long waits passed");
}
