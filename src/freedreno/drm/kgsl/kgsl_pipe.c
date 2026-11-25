/*
 * Copyright © 2023 Lucas Fryzek <lfryzek@igalia.com>
 * SPDX-License-Identifier: MIT
 */

#include "util/os_time.h"

#include "freedreno_ringbuffer_sp.h"

#include "kgsl_priv.h"

/* TODO borrowed from turnip, should be shared somewhere common. */
int
kgsl_safe_ioctl(int fd, unsigned long request, void *arg)
{
   int ret;

   do {
      ret = ioctl(fd, request, arg);
   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

   return ret;
}

#include "kgsl_wait.h"

int
kgsl_get_prop(int fd, unsigned int type, void *value, size_t size)
{
   struct kgsl_device_getproperty getprop = {
      .type = type,
      .value = value,
      .sizebytes = size,
   };

   return kgsl_safe_ioctl(fd, IOCTL_KGSL_DEVICE_GETPROPERTY, &getprop);
}

static int
kgsl_pipe_get_param(struct fd_pipe *pipe, enum fd_param_id param,
                    uint64_t *value)
{
   struct kgsl_pipe *kgsl_pipe = to_kgsl_pipe(pipe);

   switch (param) {
   case FD_DEVICE_ID:
   case FD_GPU_ID:
      *value = kgsl_pipe->dev_id.gpu_id;
      return 0;
   case FD_GMEM_SIZE:
      *value = kgsl_pipe->gmem_size;
      return 0;
   case FD_GMEM_BASE:
      *value = kgsl_pipe->gmem_base;
      return 0;
   case FD_CHIP_ID:
      *value = kgsl_pipe->dev_id.chip_id;
      return 0;
   case FD_NR_PRIORITIES:
      /* Taken from the kgsl kmd sources: a4xx and newer have
       * KGSL_PRIORITY_MAX_RB_LEVELS=4 priorities, older ones just one.
       */
      *value = kgsl_pipe->dev_id.gpu_id >= 400 ? 4 : 1;
      return 0;
   case FD_MAX_FREQ:
   case FD_TIMESTAMP:
      /* Fail these explicitly rather than falling through to the default
       * case: there is no way to convert a kgsl timestamp into a time
       * value, so this is an unsupported feature and not an error worth
       * logging on every call.
       */
      return -1;
   default:
      ERROR_MSG("invalid param id: %d", param);
      return -1;
   }
}

static int
kgsl_pipe_set_param(struct fd_pipe *pipe, enum fd_param_id param,
                    uint64_t value)
{
   ERROR_MSG("invalid param id: %d", param);
   return -1;
}

static int
kgsl_pipe_wait(struct fd_pipe *pipe, const struct fd_fence *fence,
               uint64_t timeout)
{
   struct kgsl_pipe *kgsl_pipe = to_kgsl_pipe(pipe);

   /* fd_pipe_wait() hands us a relative timeout in nanoseconds, while the
    * helper below works on an absolute deadline (so restarted waits do not
    * extend it).
    */
   return wait_timestamp_safe(pipe->dev->fd, kgsl_pipe->queue_id,
                              fence->kfence,
                              os_time_get_absolute_timeout(timeout));
}

static int
kgsl_pipe_reset_status(struct fd_pipe *pipe, enum fd_reset_status *status)
{
   struct kgsl_pipe *kgsl_pipe = to_kgsl_pipe(pipe);

   /* The context id is passed in, and the reset status comes back out, in
    * the same word.
    */
   uint32_t value = kgsl_pipe->queue_id;

   int ret = kgsl_get_prop(pipe->dev->fd, KGSL_PROP_GPU_RESET_STAT, &value,
                           sizeof(value));
   if (ret)
      return ret;

   switch (value) {
   case KGSL_CTX_STAT_NO_ERROR:
      *status = FD_RESET_NO_ERROR;
      break;
   case KGSL_CTX_STAT_GUILTY_CONTEXT_RESET_EXT:
      *status = FD_RESET_GUILTY;
      break;
   case KGSL_CTX_STAT_INNOCENT_CONTEXT_RESET_EXT:
      *status = FD_RESET_INNOCENT;
      break;
   case KGSL_CTX_STAT_UNKNOWN_CONTEXT_RESET_EXT:
   default:
      *status = FD_RESET_UNKNOWN;
      break;
   }

   return 0;
}

static void
kgsl_pipe_destroy(struct fd_pipe *pipe)
{
   struct kgsl_pipe *kgsl_pipe = to_kgsl_pipe(pipe);
   struct kgsl_drawctxt_destroy req = {
      .drawctxt_id = kgsl_pipe->queue_id,
   };

   fd_pipe_sp_ringpool_fini(pipe);
   kgsl_safe_ioctl(pipe->dev->fd, IOCTL_KGSL_DRAWCTXT_DESTROY, &req);

   free(kgsl_pipe);
}

static const struct fd_pipe_funcs funcs = {
   .ringbuffer_new_object = fd_ringbuffer_sp_new_object,
   .submit_new = kgsl_submit_sp_new,
   .reset_status = kgsl_pipe_reset_status,
   .flush = fd_pipe_sp_flush,
   .get_param = kgsl_pipe_get_param,
   .set_param = kgsl_pipe_set_param,
   .wait = kgsl_pipe_wait,
   .destroy = kgsl_pipe_destroy,
};

struct fd_pipe *
kgsl_pipe_new(struct fd_device *dev, enum fd_pipe_id id, unsigned prio)
{
   struct kgsl_pipe *kgsl_pipe = calloc(1, sizeof(*kgsl_pipe));
   if (!kgsl_pipe) {
      ERROR_MSG("allocation failed");
      return NULL;
   }

   struct fd_pipe *pipe = &kgsl_pipe->base;
   pipe->dev = dev;
   pipe->funcs = &funcs;

   struct kgsl_devinfo info;
   if (kgsl_get_prop(dev->fd, KGSL_PROP_DEVICE_INFO, &info, sizeof(info))) {
      ERROR_MSG("KGSL_PROP_DEVICE_INFO failed (%s)", strerror(errno));
      goto fail;
   }

   uint64_t gmem_iova;
   if (kgsl_get_prop(dev->fd, KGSL_PROP_UCHE_GMEM_VADDR, &gmem_iova,
                     sizeof(gmem_iova))) {
      ERROR_MSG("KGSL_PROP_UCHE_GMEM_VADDR failed (%s)", strerror(errno));
      goto fail;
   }

   kgsl_pipe->dev_id.gpu_id = ((info.chip_id >> 24) & 0xff) * 100 +
                              ((info.chip_id >> 16) & 0xff) * 10 +
                              ((info.chip_id >> 8) & 0xff);
   kgsl_pipe->dev_id.chip_id = info.chip_id;
   kgsl_pipe->gmem_size = info.gmem_sizebytes;
   kgsl_pipe->gmem_base = gmem_iova;

   struct kgsl_drawctxt_create req = {
      .flags = KGSL_CONTEXT_SAVE_GMEM | KGSL_CONTEXT_NO_GMEM_ALLOC |
               KGSL_CONTEXT_PREAMBLE,
   };

   /* Gallium uses lower numeric priorities for higher priority contexts.
    * KGSL reserves zero for the kernel-selected default (normally 8).
    * Leave the normal and internal probe contexts at that default.
    */
   const unsigned base_flags = req.flags;
   if (prio == 0)
      req.flags |= 1u << KGSL_CONTEXT_PRIORITY_SHIFT;
   else if (prio >= 3)
      req.flags |= 15u << KGSL_CONTEXT_PRIORITY_SHIFT;

   int ret = kgsl_safe_ioctl(dev->fd, IOCTL_KGSL_DRAWCTXT_CREATE, &req);
   if (ret && req.flags != base_flags &&
       (errno == EINVAL || errno == EPERM || errno == EACCES)) {
      req = (struct kgsl_drawctxt_create) { .flags = base_flags };
      ret = kgsl_safe_ioctl(dev->fd, IOCTL_KGSL_DRAWCTXT_CREATE, &req);
   }
   if (ret) {
      ERROR_MSG("DRAWCTXT_CREATE failed (%s)", strerror(errno));
      goto fail;
   }

   kgsl_pipe->queue_id = req.drawctxt_id;

   fd_pipe_sp_ringpool_init(pipe);

   return pipe;

fail:
   /* The drawctxt is only created as the last step, so nothing has been
    * handed to the kernel yet at this point.
    */
   free(kgsl_pipe);
   return NULL;
}
