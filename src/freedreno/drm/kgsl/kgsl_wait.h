/* SPDX-License-Identifier: MIT */
#ifndef KGSL_WAIT_H
#define KGSL_WAIT_H

/* KGSL interprets a zero timeout as forever. An expired deadline must poll,
 * including when a previously finite wait is interrupted. Keep this independent
 * of DRM syncobjs: the custom backend submits KGSL context timestamps.
 */
static int
kgsl_poll_timestamp(int fd, unsigned context_id, unsigned timestamp)
{
   struct kgsl_cmdstream_readtimestamp_ctxtid read = {
      .context_id = context_id,
      .type = KGSL_TIMESTAMP_RETIRED,
   };
   int ret = kgsl_safe_ioctl(fd, IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID, &read);
   if (ret)
      return ret;
   if ((int32_t)(read.timestamp - timestamp) >= 0)
      return 0;
   errno = ETIMEDOUT;
   return -1;
}

static int
wait_timestamp_safe(int fd, unsigned context_id, unsigned timestamp,
                    uint64_t abs_timeout_ns)
{
   struct kgsl_device_waittimestamp_ctxtid wait = {
      .context_id = context_id,
      .timestamp = timestamp,
   };
   while (true) {
      if (abs_timeout_ns >= INT64_MAX) {
         wait.timeout = (unsigned)-1;
      } else {
         uint64_t now_ms = os_time_get_nano() / 1000000;
         uint64_t deadline_ms = abs_timeout_ns / 1000000;
         if (deadline_ms <= now_ms)
            return kgsl_poll_timestamp(fd, context_id, timestamp);
         /* Prevent overflow from turning a long finite deadline into forever. */
         uint64_t remaining = deadline_ms - now_ms;
         wait.timeout = remaining > INT_MAX ? INT_MAX : (unsigned)remaining;
      }
      int ret = ioctl(fd, IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID, &wait);
      if (ret == 0 || (errno != EINTR && errno != EAGAIN))
         return ret;
   }
}
#endif
