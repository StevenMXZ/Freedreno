/*
 * Copyright © 2023 Lucas Fryzek <lfryzek@igalia.com>
 * SPDX-License-Identifier: MIT
 */

#include <fcntl.h>

#include "../../vulkan/ion/ion.h"
#include "../../vulkan/ion/ion_4.19.h"

#include "kgsl_priv.h"

static void
kgsl_device_destroy(struct fd_device *dev)
{
   struct kgsl_device *kgsl_dev = to_kgsl_device(dev);

   if (kgsl_dev->dma_fd >= 0)
      close(kgsl_dev->dma_fd);
}

static const struct fd_device_funcs funcs = {
   .bo_new = kgsl_bo_new,
#if DETECT_OS_ANDROID
   .bo_new_shared_image = kgsl_bo_new_shared_image,
#endif
   .bo_from_handle = kgsl_bo_from_handle,
   .bo_from_dmabuf = kgsl_bo_from_dmabuf,
   .bo_close_handle = kgsl_bo_close_handle,
   .pipe_new = kgsl_pipe_new,
   .destroy = kgsl_device_destroy,
};

/**
 * kgsl can import dma-bufs but cannot allocate or export them, so shareable
 * buffers come from a dma-buf heap (or ION on older kernels).  Work out which
 * of those this kernel offers.  Mirrors turnip's probe in tu_knl_kgsl.cc.
 */
static void
kgsl_probe_dma_heap(struct kgsl_device *kgsl_dev)
{
   static const char dma_heap_path[] = "/dev/dma_heap/system";
   static const char ion_path[] = "/dev/ion";

   kgsl_dev->dma_fd = open(dma_heap_path, O_RDONLY | O_CLOEXEC);
   if (kgsl_dev->dma_fd >= 0) {
      kgsl_dev->dma_type = KGSL_DMA_TYPE_DMAHEAP;
      return;
   }

   kgsl_dev->dma_fd = open(ion_path, O_RDONLY | O_CLOEXEC);
   if (kgsl_dev->dma_fd >= 0) {
      /* ION_IOC_FREE is only available on legacy ION: */
      struct ion_handle_data free = { .handle = 0 };
      if (kgsl_safe_ioctl(kgsl_dev->dma_fd, ION_IOC_FREE, &free) >= 0 ||
          errno != ENOTTY)
         kgsl_dev->dma_type = KGSL_DMA_TYPE_ION_LEGACY;
      else
         kgsl_dev->dma_type = KGSL_DMA_TYPE_ION;
      return;
   }

   kgsl_dev->dma_type = KGSL_DMA_TYPE_NONE;
   INFO_MSG("could not open %s or %s, shared buffers will be unavailable: %s",
            dma_heap_path, ion_path, strerror(errno));
}

/* Probe both the allocation and returned flags; some KGSL kernels silently
 * discard IOCOHERENT. Keep write-combine as the default for unsupported GPUs.
 */
static bool
kgsl_probe_cached_coherent(int fd)
{
   const unsigned flags = KGSL_MEMFLAGS_IOCOHERENT |
                          (KGSL_CACHEMODE_WRITEBACK << KGSL_CACHEMODE_SHIFT);
   struct kgsl_gpumem_alloc_id req = {
      .size = 4096,
      .flags = flags,
   };
   if (kgsl_safe_ioctl(fd, IOCTL_KGSL_GPUMEM_ALLOC_ID, &req))
      return false;

   bool supported = (req.flags & (KGSL_MEMFLAGS_IOCOHERENT |
                                  KGSL_CACHEMODE_MASK)) == flags;
   struct kgsl_gpumem_free_id free_req = { .id = req.id };
   if (kgsl_safe_ioctl(fd, IOCTL_KGSL_GPUMEM_FREE_ID, &free_req))
      return false;

   return supported;
}

struct fd_device *
kgsl_device_new(int fd)
{
   struct kgsl_device *kgsl_dev;
   struct fd_device *dev;
   struct kgsl_devinfo info;

   /* Probe the device info to check whether this fd really is kgsl.  This is
    * the only thing distinguishing it from an arbitrary character device,
    * since kgsl answers no DRM ioctl.
    */
   if (kgsl_get_prop(fd, KGSL_PROP_DEVICE_INFO, &info, sizeof(info)))
      return NULL;

   kgsl_dev = calloc(1, sizeof(*kgsl_dev));
   if (!kgsl_dev)
      return NULL;

   dev = &kgsl_dev->base;
   dev->funcs = &funcs;
   dev->fd = fd;
   dev->version = FD_VERSION_ROBUSTNESS;
   dev->features = FD_FEATURE_DIRECT_RESET | FD_FEATURE_IMPORT_DMABUF;
#if DETECT_OS_ANDROID
   dev->features |= FD_FEATURE_SHARED_IMAGE;
#endif
   dev->bo_size = sizeof(struct kgsl_bo);

   kgsl_probe_dma_heap(kgsl_dev);
   dev->has_cached_coherent = kgsl_probe_cached_coherent(fd);

   /* async submit_queue used for softpin deferred submits */
   util_queue_init(&dev->submit_queue, "sq", 8, 1, 0, NULL);

   return dev;
}
