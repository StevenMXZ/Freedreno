/*
 * Copyright © 2023 Lucas Fryzek <lfryzek@igalia.com>
 * SPDX-License-Identifier: MIT
 */

#ifndef KGSL_PRIV_H_
#define KGSL_PRIV_H_

#include "freedreno_priv.h"

#ifndef __user
#define __user
#endif

/* TODO the KGSL kernel interface should probably be moved into someplace
 * common that both turnip and freedreno can use, rather than reaching into
 * the vulkan driver like this.
 */
#include "../../vulkan/msm_kgsl.h"

/* ION_HEAP_SYSTEM, as used by the downstream msm ION driver. */
#define KGSL_ION_SYSTEM_HEAP_MASK (1u << 25)

/**
 * Which ABI the kernel offers for allocating the shareable (dma-buf backed)
 * memory that kgsl itself cannot allocate.  Probed once at device creation.
 */
enum kgsl_dma_type {
   KGSL_DMA_TYPE_NONE = 0,
   KGSL_DMA_TYPE_DMAHEAP,
   KGSL_DMA_TYPE_ION,
   KGSL_DMA_TYPE_ION_LEGACY,
};

struct kgsl_device {
   struct fd_device base;

   /* fd of /dev/dma_heap/system or /dev/ion, or -1 if neither is available.
    * Kept open for the lifetime of the device rather than reopened per
    * allocation.
    */
   int dma_fd;
   enum kgsl_dma_type dma_type;
};
FD_DEFINE_CAST(fd_device, kgsl_device);

struct fd_device *kgsl_device_new(int fd);

struct kgsl_pipe {
   struct fd_pipe base;

   struct fd_dev_id dev_id;

   uint32_t gmem_size;
   uint64_t gmem_base;
   uint32_t queue_id;
};
FD_DEFINE_CAST(fd_pipe, kgsl_pipe);

struct fd_pipe *kgsl_pipe_new(struct fd_device *dev, enum fd_pipe_id id,
                              unsigned prio);

struct kgsl_bo {
   struct fd_bo base;
   uint64_t iova;

   /* For KGSL_BO_IMPORT, the dma-buf fd backing this BO.  kgsl has no
    * export ioctl, so this is also what fd_bo_dmabuf() hands out.  -1 for
    * natively allocated BOs, which are simply not shareable.
    */
   int import_fd;

   enum {
      KGSL_BO_NATIVE,
      KGSL_BO_IMPORT,
   } bo_type;
};
FD_DEFINE_CAST(fd_bo, kgsl_bo);

struct fd_bo *kgsl_bo_new(struct fd_device *dev, uint32_t size, uint32_t flags);
struct fd_bo *kgsl_bo_from_handle(struct fd_device *dev, uint32_t size,
                                  uint32_t handle);
struct fd_bo *kgsl_bo_from_dmabuf(struct fd_device *dev, int fd);
void kgsl_bo_close_handle(struct fd_bo *bo);

struct fd_submit *kgsl_submit_sp_new(struct fd_pipe *pipe);

int kgsl_safe_ioctl(int fd, unsigned long request, void *arg);
int kgsl_get_prop(int fd, unsigned int type, void *value, size_t size);

#endif /* KGSL_PRIV_H_ */
