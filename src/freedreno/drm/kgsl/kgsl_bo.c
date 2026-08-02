/*
 * Copyright © 2023 Lucas Fryzek <lfryzek@igalia.com>
 * SPDX-License-Identifier: MIT
 */

#include <fcntl.h>
#include <linux/dma-heap.h>

#include "util/os_file.h"
#include "util/os_mman.h"

#include "../../vulkan/ion/ion.h"
#include "../../vulkan/ion/ion_4.19.h"

#include "kgsl_priv.h"

#if DETECT_OS_ANDROID
#include <android/hardware_buffer.h>
#include <sys/socket.h>

#include "drm-uapi/drm_fourcc.h"

#include <dlfcn.h>

#include "util/u_call_once.h"

/* AHardwareBuffer_getNativeHandle() lives in libandroid.so and has since API
 * 26, but it is declared only by <vndk/hardware_buffer.h>, an AOSP header the
 * NDK does not ship.  turnip reaches it through the VNDK sysroot; here there
 * is none, so declare the little of it that is needed.
 */
typedef struct native_handle {
   int version;
   int numFds;
   int numInts;
   int data[0];
} native_handle_t;

/* gralloc has always known this one, but the NDK enum omits it. It is what
 * Winlator's X server allocates for its own drawables.
 */
#define HAL_PIXEL_FORMAT_BGRA_8888 5

/*
 * Resolved at first use rather than linked against: a DT_NEEDED on
 * libandroid.so stops this library from loading inside the Winlator container,
 * where the linker walks its chain to /system/lib64/libsqlite.so and fails to
 * resolve that library's OpenSSL imports against the container's libcrypto.
 *
 * Not finding them is not fatal; the caller falls back to a dma-buf heap.
 */
static struct {
   int (*allocate)(const AHardwareBuffer_Desc *desc, AHardwareBuffer **out);
   void (*describe)(const AHardwareBuffer *buffer, AHardwareBuffer_Desc *desc);
   void (*release)(AHardwareBuffer *buffer);
   const native_handle_t *(*get_native_handle)(const AHardwareBuffer *buffer);
   int (*send_handle)(const AHardwareBuffer *buffer, int fd);
} ahb;

static void *
ahb_sym(void *const *libs, unsigned nlibs, const char *name)
{
   for (unsigned i = 0; i < nlibs; i++) {
      void *sym = dlsym(libs[i], name);
      if (sym)
         return sym;
   }

   WARN_MSG("no %s in any AHardwareBuffer provider", name);
   return NULL;
}

static void
ahb_load_once(void)
{
   static const char *const names[] = {
      /* Where these actually live.  Tried first because libandroid.so only
       * forwards to it, behind a much larger dependency chain.
       */
      "libnativewindow.so",
      /* Still worth a try: it is the only place the NDK stub carries
       * getNativeHandle(), which is a VNDK entry point.
       */
      "libandroid.so",
   };
   void *libs[ARRAY_SIZE(names)];
   unsigned nlibs = 0;

   for (unsigned i = 0; i < ARRAY_SIZE(names); i++) {
      /* Either one missing is unremarkable -- only having neither is a
       * problem, and ahb_available() reports that below.
       */
      void *lib = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);

      if (lib)
         libs[nlibs++] = lib;
   }

   if (!nlibs) {
      WARN_MSG("no AHardwareBuffer provider; shared images unavailable");
      return;
   }

   ahb.describe = (void (*)(const AHardwareBuffer *, AHardwareBuffer_Desc *))ahb_sym(libs, nlibs, "AHardwareBuffer_describe");
   ahb.release = (void (*)(AHardwareBuffer *))ahb_sym(libs, nlibs, "AHardwareBuffer_release");
   ahb.get_native_handle = (const native_handle_t *(*)(const AHardwareBuffer *))ahb_sym(libs, nlibs, "AHardwareBuffer_getNativeHandle");
   ahb.send_handle = (int (*)(const AHardwareBuffer *, int))ahb_sym(libs, nlibs, "AHardwareBuffer_sendHandleToUnixSocket");

   /* Set last: it is what ahb_available() tests, so a half-resolved table
    * never looks usable.
    */
   if (ahb.describe && ahb.release && ahb.get_native_handle && ahb.send_handle)
      ahb.allocate = (int (*)(const AHardwareBuffer_Desc *, AHardwareBuffer **))ahb_sym(libs, nlibs, "AHardwareBuffer_allocate");
}

static bool
ahb_available(void)
{
   static util_once_flag once = UTIL_ONCE_FLAG_INIT;

   util_call_once(&once, ahb_load_once);

   return ahb.allocate != NULL;
}
#endif

static uint64_t
kgsl_bo_iova(struct fd_bo *bo)
{
   return to_kgsl_bo(bo)->iova;
}

static void
kgsl_bo_set_name(struct fd_bo *bo, const char *fmt, va_list ap)
{
   /* kgsl has no equivalent of MSM_INFO_SET_NAME */
}

static int
kgsl_bo_offset(struct fd_bo *bo, uint64_t *offset)
{
   /* The mmap offset of a kgsl allocation is its id shifted over by 12. */
   *offset = (uint64_t)bo->handle << 12;
   return 0;
}

static int
kgsl_bo_madvise(struct fd_bo *bo, int willneed)
{
   /* kgsl has no madvise ioctl, so pretend the kernel kept the pages. */
   return willneed;
}

static int
kgsl_bo_cpu_prep(struct fd_bo *bo, struct fd_pipe *pipe, uint32_t op)
{
   /* Only implicit sync needs handling here, which is a no-op for kgsl. */
   return 0;
}

static void *
kgsl_bo_map(struct fd_bo *bo)
{
   struct kgsl_bo *kgsl_bo = to_kgsl_bo(bo);

   if (bo->map)
      return bo->map;

   void *map;
   if (kgsl_bo->bo_type == KGSL_BO_IMPORT) {
      /* An imported BO is mapped through the dma-buf itself, not through
       * the kgsl device node.
       */
      map = os_mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    kgsl_bo->import_fd, 0);
   } else {
      uint64_t offset;

      if (bo->funcs->offset(bo, &offset))
         return NULL;

      map = os_mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    bo->dev->fd, offset);
   }

   if (map == MAP_FAILED) {
      ERROR_MSG("mmap failed: %s", strerror(errno));
      return NULL;
   }

   bo->map = map;

   return bo->map;
}

#if DETECT_OS_ANDROID
/**
 * Winlator's X server treats the fd it gets from PixmapFromBuffer(s) as a
 * socketpair, not a dma-buf: it writes a handshake byte and then blocks in
 * AHardwareBuffer_recvHandleFromUnixSocket().
 *
 * Queue the handle before the request carrying the fd is sent.  SCM_RIGHTS
 * data is buffered, so the server finds it waiting rather than stalling its
 * request thread on a reply we could not send while blocked ourselves.
 *
 * Our end stays open, or the server's handshake write takes EPIPE.
 */
static int
kgsl_bo_send_ahb(struct kgsl_bo *kgsl_bo)
{
   int sv[2];

   if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
      ERROR_MSG("socketpair failed (%s)", strerror(errno));
      return -1;
   }

   if (ahb.send_handle(kgsl_bo->ahb, sv[0])) {
      ERROR_MSG("could not queue the AHardwareBuffer handle");
      close(sv[0]);
      close(sv[1]);
      return -1;
   }

   /* A BO can be asked for a handle more than once; the pixmap built from the
    * previous one is gone by then, so only the newest sender end matters.
    */
   if (kgsl_bo->handshake_fd >= 0)
      close(kgsl_bo->handshake_fd);
   kgsl_bo->handshake_fd = sv[0];

   return sv[1];
}
#endif

static int
kgsl_bo_dmabuf(struct fd_bo *bo)
{
   struct kgsl_bo *kgsl_bo = to_kgsl_bo(bo);

#if DETECT_OS_ANDROID
   if (kgsl_bo->ahb)
      return kgsl_bo_send_ahb(kgsl_bo);
#endif

   /* kgsl has no export ioctl: a BO is only shareable if it was created
    * from a dma-buf in the first place, in which case "exporting" it is
    * just handing back another reference to that dma-buf.
    */
   if (kgsl_bo->bo_type != KGSL_BO_IMPORT || kgsl_bo->import_fd < 0) {
      ERROR_MSG("cannot export a natively allocated kgsl buffer");
      return -1;
   }

   return os_dupfd_cloexec(kgsl_bo->import_fd);
}

void
kgsl_bo_close_handle(struct fd_bo *bo)
{
   struct kgsl_bo *kgsl_bo = to_kgsl_bo(bo);

   if (kgsl_bo->import_fd >= 0) {
      close(kgsl_bo->import_fd);
      kgsl_bo->import_fd = -1;
   }

   if (kgsl_bo->handshake_fd >= 0) {
      close(kgsl_bo->handshake_fd);
      kgsl_bo->handshake_fd = -1;
   }

#if DETECT_OS_ANDROID
   if (kgsl_bo->ahb) {
      ahb.release(kgsl_bo->ahb);
      kgsl_bo->ahb = NULL;
   }
#endif

   struct kgsl_gpumem_free_id req = {
      .id = bo->handle,
   };

   kgsl_safe_ioctl(bo->dev->fd, IOCTL_KGSL_GPUMEM_FREE_ID, &req);
}

static void
kgsl_bo_destroy(struct fd_bo *bo)
{
   /* kgsl frees the allocation as soon as the id is released, with no
    * regard for whether the GPU is still reading it.  There is no
    * deferred-free or madvise mechanism to lean on, so wait out every
    * fence the BO is referenced by before letting it go.
    */
   for (uint32_t i = 0; i < bo->nr_fences; i++) {
      struct fd_pipe *pipe = bo->fences[i]->pipe;
      pipe->funcs->wait(pipe, bo->fences[i], OS_TIMEOUT_INFINITE);
   }

   fd_bo_fini_common(bo);
}

static const struct fd_bo_funcs bo_funcs = {
   .offset = kgsl_bo_offset,
   .map = kgsl_bo_map,
   .cpu_prep = kgsl_bo_cpu_prep,
   .madvise = kgsl_bo_madvise,
   .iova = kgsl_bo_iova,
   .set_name = kgsl_bo_set_name,
   .dmabuf = kgsl_bo_dmabuf,
   .destroy = kgsl_bo_destroy,
};

/**
 * Wrap an already-created kgsl allocation id in an fd_bo.
 */
static struct fd_bo *
kgsl_bo_alloc(struct fd_device *dev, uint32_t size, uint32_t handle,
              uint64_t iova, int import_fd)
{
   struct kgsl_bo *kgsl_bo = calloc(1, sizeof(*kgsl_bo));
   if (!kgsl_bo)
      return NULL;

   struct fd_bo *bo = &kgsl_bo->base;
   bo->size = size;
   bo->handle = handle;
   bo->funcs = &bo_funcs;

   kgsl_bo->iova = iova;
   kgsl_bo->import_fd = import_fd;
   kgsl_bo->handshake_fd = -1;
   kgsl_bo->bo_type = import_fd >= 0 ? KGSL_BO_IMPORT : KGSL_BO_NATIVE;

   fd_bo_init_common(bo, dev);

   return bo;
}

/* Note: size is ignored, as kgsl knows the real size of the allocation. */
struct fd_bo *
kgsl_bo_from_handle(struct fd_device *dev, uint32_t size, uint32_t handle)
{
   struct kgsl_gpuobj_info req = {
      .id = handle,
   };

   if (kgsl_safe_ioctl(dev->fd, IOCTL_KGSL_GPUOBJ_INFO, &req)) {
      ERROR_MSG("GPUOBJ_INFO failed (%s)", strerror(errno));
      return NULL;
   }

   return kgsl_bo_alloc(dev, req.size, req.id, req.gpuaddr, -1);
}

struct fd_bo *
kgsl_bo_from_dmabuf(struct fd_device *dev, int fd)
{
   struct kgsl_gpuobj_import_dma_buf import_dmabuf = {
      .fd = fd,
   };
   struct kgsl_gpuobj_import req = {
      .priv = (uintptr_t)&import_dmabuf,
      .priv_len = sizeof(import_dmabuf),
      .flags = 0,
      .type = KGSL_USER_MEM_TYPE_DMABUF,
   };

   if (kgsl_safe_ioctl(dev->fd, IOCTL_KGSL_GPUOBJ_IMPORT, &req)) {
      ERROR_MSG("GPUOBJ_IMPORT failed (%s)", strerror(errno));
      return NULL;
   }

   /* The import ioctl only hands back an id, so the size and iova still
    * have to be queried separately.
    */
   struct kgsl_gpuobj_info info = {
      .id = req.id,
   };

   if (kgsl_safe_ioctl(dev->fd, IOCTL_KGSL_GPUOBJ_INFO, &info)) {
      ERROR_MSG("GPUOBJ_INFO failed for imported dma-buf (%s)",
                strerror(errno));
      goto fail_free_id;
   }

   int import_fd = os_dupfd_cloexec(fd);
   if (import_fd < 0) {
      ERROR_MSG("failed to dup dma-buf fd (%s)", strerror(errno));
      goto fail_free_id;
   }

   struct fd_bo *bo =
      kgsl_bo_alloc(dev, info.size, req.id, info.gpuaddr, import_fd);
   if (!bo) {
      close(import_fd);
      goto fail_free_id;
   }

   return bo;

fail_free_id: {
   struct kgsl_gpumem_free_id free_req = {
      .id = req.id,
   };
   kgsl_safe_ioctl(dev->fd, IOCTL_KGSL_GPUMEM_FREE_ID, &free_req);
   return NULL;
}
}

/**
 * kgsl cannot allocate dma-bufs itself, so shareable buffers are allocated
 * from a dma-buf heap (or ION on older kernels) and then imported.
 */
static int
kgsl_dma_heap_alloc(struct kgsl_device *kgsl_dev, uint32_t size)
{
   int ret;

   switch (kgsl_dev->dma_type) {
   case KGSL_DMA_TYPE_DMAHEAP: {
      struct dma_heap_allocation_data alloc = {
         .len = size,
         .fd_flags = O_RDWR | O_CLOEXEC,
      };

      ret = kgsl_safe_ioctl(kgsl_dev->dma_fd, DMA_HEAP_IOCTL_ALLOC, &alloc);
      if (ret) {
         ERROR_MSG("DMA_HEAP_IOCTL_ALLOC failed (%s)", strerror(errno));
         return -1;
      }

      return alloc.fd;
   }

   case KGSL_DMA_TYPE_ION: {
      struct ion_new_allocation_data alloc = {
         .len = size,
         .heap_id_mask = KGSL_ION_SYSTEM_HEAP_MASK,
         .flags = 0,
         .fd = -1,
      };

      ret = kgsl_safe_ioctl(kgsl_dev->dma_fd, ION_IOC_NEW_ALLOC, &alloc);
      if (ret) {
         ERROR_MSG("ION_IOC_NEW_ALLOC failed (%s)", strerror(errno));
         return -1;
      }

      return alloc.fd;
   }

   case KGSL_DMA_TYPE_ION_LEGACY: {
      struct ion_allocation_data alloc = {
         .len = size,
         .align = 4096,
         .heap_id_mask = KGSL_ION_SYSTEM_HEAP_MASK,
         .flags = 0,
      };

      ret = kgsl_safe_ioctl(kgsl_dev->dma_fd, ION_IOC_ALLOC, &alloc);
      if (ret) {
         ERROR_MSG("ION_IOC_ALLOC failed (%s)", strerror(errno));
         return -1;
      }

      struct ion_fd_data share = {
         .handle = alloc.handle,
      };

      ret = kgsl_safe_ioctl(kgsl_dev->dma_fd, ION_IOC_SHARE, &share);
      if (ret) {
         ERROR_MSG("ION_IOC_SHARE failed (%s)", strerror(errno));
         return -1;
      }

      return share.fd;
   }

   case KGSL_DMA_TYPE_NONE:
   default:
      ERROR_MSG("no dma-buf heap available, cannot allocate a shared buffer");
      return -1;
   }
}

static struct fd_bo *
kgsl_bo_new_dmabuf(struct fd_device *dev, uint32_t size)
{
   struct kgsl_device *kgsl_dev = to_kgsl_device(dev);

   int fd = kgsl_dma_heap_alloc(kgsl_dev, size);
   if (fd < 0)
      return NULL;

   /* kgsl_bo_from_dmabuf() keeps a dup of its own, so the fd is ours to
    * close either way.
    */
   struct fd_bo *bo = kgsl_bo_from_dmabuf(dev, fd);
   close(fd);

   return bo;
}

#if DETECT_OS_ANDROID
static uint32_t
ahb_format_from_fourcc(uint32_t fourcc, uint32_t *cpp)
{
   *cpp = 4;

   switch (fourcc) {
   case DRM_FORMAT_ABGR8888:
      return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
   case DRM_FORMAT_XBGR8888:
      return AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM;
   case DRM_FORMAT_ARGB8888:
   case DRM_FORMAT_XRGB8888:
      return HAL_PIXEL_FORMAT_BGRA_8888;
   case DRM_FORMAT_RGB565:
      *cpp = 2;
      return AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM;
   default:
      return 0;
   }
}

struct fd_bo *
kgsl_bo_new_shared_image(struct fd_device *dev, uint32_t width, uint32_t height,
                         uint32_t fourcc, uint32_t *pitch)
{
   uint32_t cpp;
   uint32_t format = ahb_format_from_fourcc(fourcc, &cpp);

   /* Not an error: the caller falls back to an ordinary allocation. */
   if (!format)
      return NULL;

   if (!ahb_available())
      return NULL;

   /* fdl6_layout_image() pads the last level's height to a multiple of four --
    * the mem<->gmem blits work at 16x4 granularity and over-fetch past the
    * bottom edge -- and then refuses any buffer smaller than the padded size,
    * which gralloc has no reason to have left room for.  Ask for the rows up
    * front rather than fall back to a buffer no compositor here will take.
    *
    * The buffer ends up a couple of rows taller than the image, which shows as
    * a slight vertical squash in a consumer that samples its full height.
    */
   AHardwareBuffer_Desc desc = {
      .width = width,
      .height = align(height, 4),
      .layers = 1,
      .format = format,
      /* Without COMPOSER_OVERLAY SurfaceFlinger will not scan the buffer out
       * directly and composes it through the GPU instead.
       *
       * The CPU bits are for the consumer, not for us -- nothing here maps the
       * buffer.  The X server answers a CopyArea out of a DRI3 pixmap by
       * locking it for CPU_READ and running a memcpy; on a GPU-only buffer
       * AHardwareBuffer_lock() still succeeds but the mapping is not
       * CPU-readable, so that memcpy faults with SEGV_ACCERR and takes the
       * server down, reaching us only as the X connection dropping.
       *
       * They also keep gralloc off a compressed layout, which suits this path:
       * it asks for DRM_FORMAT_MOD_LINEAR anyway.
       */
      .usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
               AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
               AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY |
               AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
               AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
   };

   AHardwareBuffer *ahbuf = NULL;
   if (ahb.allocate(&desc, &ahbuf)) {
      ERROR_MSG("AHardwareBuffer_allocate failed for %ux%u fourcc 0x%08x",
                width, height, fourcc);
      return NULL;
   }

   /* Re-describe: the allocator chose the pitch, and the layout above us has
    * to be built around it rather than the other way round.
    */
   ahb.describe(ahbuf, &desc);

   const native_handle_t *handle = ahb.get_native_handle(ahbuf);
   if (!handle || handle->numFds < 1) {
      ERROR_MSG("AHardwareBuffer carries no dma-buf to import");
      ahb.release(ahbuf);
      return NULL;
   }

   /* kgsl_bo_from_dmabuf() dups what it keeps, and the AHardwareBuffer still
    * owns the fd in the native handle, so neither side has to close it here.
    */
   struct fd_bo *bo = kgsl_bo_from_dmabuf(dev, handle->data[0]);
   if (!bo) {
      ahb.release(ahbuf);
      return NULL;
   }

   to_kgsl_bo(bo)->ahb = ahbuf;

   /* gralloc reports the stride in pixels; every caller wants bytes. */
   *pitch = desc.stride * cpp;

   return bo;
}
#endif

struct fd_bo *
kgsl_bo_new(struct fd_device *dev, uint32_t size, uint32_t flags)
{
   if (flags & (FD_BO_SHARED | FD_BO_SCANOUT))
      return kgsl_bo_new_dmabuf(dev, size);

   struct kgsl_gpumem_alloc_id req = {
      .size = size,
   };

   if (flags & FD_BO_GPUREADONLY)
      req.flags |= KGSL_MEMFLAGS_GPUREADONLY;

   /* Note that dev->has_cached_coherent is never set for kgsl, so the
    * common layer strips FD_BO_CACHED_COHERENT before we get here and the
    * writecombine path is always taken.  Enabling it would need a runtime
    * probe that KGSL_MEMFLAGS_IOCOHERENT actually works on this kernel.
    */
   if (flags & FD_BO_CACHED_COHERENT)
      req.flags |= KGSL_MEMFLAGS_IOCOHERENT |
                   (KGSL_CACHEMODE_WRITEBACK << KGSL_CACHEMODE_SHIFT);
   else
      req.flags |= KGSL_CACHEMODE_WRITECOMBINE << KGSL_CACHEMODE_SHIFT;

   if (kgsl_safe_ioctl(dev->fd, IOCTL_KGSL_GPUMEM_ALLOC_ID, &req)) {
      ERROR_MSG("GPUMEM_ALLOC_ID failed (%s)", strerror(errno));
      return NULL;
   }

   /* Unlike the import path, ALLOC_ID already reports the address and the
    * (page aligned) mappable size, so no GPUOBJ_INFO round trip is needed.
    */
   struct fd_bo *bo = kgsl_bo_alloc(dev, req.mmapsize, req.id, req.gpuaddr, -1);
   if (!bo) {
      struct kgsl_gpumem_free_id free_req = {
         .id = req.id,
      };
      kgsl_safe_ioctl(dev->fd, IOCTL_KGSL_GPUMEM_FREE_ID, &free_req);
      return NULL;
   }

   return bo;
}
