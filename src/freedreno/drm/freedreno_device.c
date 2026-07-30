/*
 * Copyright © 2012-2018 Rob Clark <robclark@freedesktop.org>
 * SPDX-License-Identifier: MIT
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "util/os_file.h"
#include "util/u_process.h"

#include "freedreno_rd_output.h"

#include "freedreno_drmif.h"
#include "freedreno_drm_perfetto.h"
#include "freedreno_priv.h"

struct fd_device *msm_device_new(int fd, drmVersionPtr version);
#ifdef HAVE_FREEDRENO_VIRTIO
struct fd_device *virtio_device_new(int fd, drmVersionPtr version);
#endif
#ifdef HAVE_FREEDRENO_KGSL
struct fd_device *kgsl_device_new(int fd);
/* Same node turnip opens; kgsl exposes no way to enumerate it. */
#define FD_KGSL_DEVICE_PATH "/dev/kgsl-3d0"
#endif

uint64_t os_page_size = 4096;

struct fd_device *
fd_device_new(int fd)
{
   struct fd_device *dev = NULL;
   drmVersionPtr version = NULL;
   bool use_heap = false;
   bool supports_heap = true;
   int dev_fd = fd;

   os_get_page_size(&os_page_size);

#ifdef HAVE_LIBDRM
   /* figure out which drm driver we are talking to, if any: */
   version = drmGetVersion(fd);
   if (!version)
      DEBUG_MSG("cannot get drm version: %s", strerror(errno));
#endif

   /* Note that kgsl is not a DRM driver at all, so drmGetVersion() fails on
    * it.  A NULL version is therefore not fatal: it is the normal case when
    * the fd refers to /dev/kgsl-3d0, which the kgsl backend probes for below.
    */

#ifdef HAVE_FREEDRENO_VIRTIO
   if (debug_get_bool_option("FD_FORCE_VTEST", false)) {
      DEBUG_MSG("virtio_gpu vtest device");
      dev = virtio_device_new(-1, version);
   } else
#endif
   if (version && !strcmp(version->name, "msm")) {
      DEBUG_MSG("msm DRM device");
      if (version->version_major != 1) {
         ERROR_MSG("unsupported version: %u.%u.%u", version->version_major,
                   version->version_minor, version->version_patchlevel);
         goto out;
      }

      dev = msm_device_new(fd, version);
#ifdef HAVE_FREEDRENO_VIRTIO
   } else if (version && !strcmp(version->name, "virtio_gpu")) {
      DEBUG_MSG("virtio_gpu DRM device");
      dev = virtio_device_new(fd, version);
      /* Only devices that support a hypervisor are a6xx+, so avoid the
       * extra guest<->host round trips associated with pipe creation:
       */
      use_heap = true;
#endif
#ifdef HAVE_FREEDRENO_KGSL
   } else {
      /* No DRM driver matched, so this may be a kgsl device node.
       * kgsl_device_new() probes with KGSL_PROP_DEVICE_INFO and returns NULL
       * if the fd is not actually kgsl.
       */
      DEBUG_MSG("probing for kgsl device");
      dev = kgsl_device_new(fd);

      if (!dev) {
         /* The fd we were handed is not kgsl, which is the normal case under an
          * X server whose DRI3 Open does not return a device fd (Winlator
          * answers with a memfd).  Open the node ourselves instead of giving up.
          *
          * Only in this branch do we take ownership of the caller's fd.  Both
          * in-tree callers are fine with that.
          */
         int kgsl_fd = open(FD_KGSL_DEVICE_PATH, O_RDWR | O_CLOEXEC);

         if (kgsl_fd >= 0) {
            dev = kgsl_device_new(kgsl_fd);
            if (dev) {
               DEBUG_MSG("opened %s directly", FD_KGSL_DEVICE_PATH);
               close(fd);
               dev_fd = kgsl_fd;
            } else {
               close(kgsl_fd);
            }
         } else {
            DEBUG_MSG("cannot open %s: %s", FD_KGSL_DEVICE_PATH,
                      strerror(errno));
         }
      }

      /* kgsl has no userspace-fence support, which the bo heaps rely on: */
      supports_heap = false;
#endif
   }

   if (!dev) {
      INFO_MSG("unsupported device: %s", version ? version->name : "non-drm");
      goto out;
   }

out:
#ifdef HAVE_LIBDRM
   drmFreeVersion(version);
#endif

   if (!dev)
      return NULL;

   fd_drm_perfetto_init();

   fd_rd_dump_env_init();
   fd_rd_output_init(&dev->rd, util_get_process_name());

   p_atomic_set(&dev->refcnt, 1);
   dev->fd = dev_fd;
   dev->handle_table =
      _mesa_hash_table_create(NULL, _mesa_hash_u32, _mesa_key_u32_equal);
   dev->name_table =
      _mesa_hash_table_create(NULL, _mesa_hash_u32, _mesa_key_u32_equal);
   fd_bo_cache_init(&dev->bo_cache, false, "bo");
   fd_bo_cache_init(&dev->ring_cache, true, "ring");

   list_inithead(&dev->deferred_submits);
   simple_mtx_init(&dev->submit_lock, mtx_plain);
   simple_mtx_init(&dev->suballoc_lock, mtx_plain);

   if (supports_heap && !use_heap) {
      struct fd_pipe *pipe = fd_pipe_new(dev, FD_PIPE_3D);

      if (!pipe)
         goto fail;

      /* Userspace fences don't appear to be reliable enough (missing some
       * cache flushes?) on older gens, so limit sub-alloc heaps to a6xx+
       * for now:
       */
      use_heap = fd_dev_gen(&pipe->dev_id) >= 6;

      fd_pipe_del(pipe);
   }

   if (supports_heap && use_heap) {
      dev->ring_heap = fd_bo_heap_new(dev, RING_FLAGS);
      dev->default_heap = fd_bo_heap_new(dev, 0);
   }

   return dev;

fail:
   fd_device_del(dev);
   return NULL;
}

/* like fd_device_new() but creates it's own private dup() of the fd
 * which is close()d when the device is finalized.
 */
struct fd_device *
fd_device_new_dup(int fd)
{
   int dup_fd = os_dupfd_cloexec(fd);
   struct fd_device *dev = fd_device_new(dup_fd);
   if (dev)
      dev->closefd = 1;
   else
      close(dup_fd);
   return dev;
}

/* Convenience helper to open the drm device and return new fd_device:
 */
struct fd_device *
fd_device_open(void)
{
   int fd;

   fd = drmOpenWithType("msm", NULL, DRM_NODE_RENDER);
#ifdef HAVE_FREEDRENO_VIRTIO
   if (fd < 0)
      fd = drmOpenWithType("virtio_gpu", NULL, DRM_NODE_RENDER);
#endif
   if (fd < 0)
      return NULL;

   return fd_device_new(fd);
}

struct fd_device *
fd_device_ref(struct fd_device *dev)
{
   ref(&dev->refcnt);
   return dev;
}

void
fd_device_purge(struct fd_device *dev)
{
   fd_bo_cache_cleanup(&dev->bo_cache, 0);
   fd_bo_cache_cleanup(&dev->ring_cache, 0);
}

void
fd_device_del(struct fd_device *dev)
{
   if (!unref(&dev->refcnt))
      return;

   fd_rd_output_fini(&dev->rd);

   assert(list_is_empty(&dev->deferred_submits));
   assert(!dev->deferred_submits_fence);

   if (dev->suballoc_bo)
      fd_bo_del(dev->suballoc_bo);

   if (dev->ring_heap)
      fd_bo_heap_destroy(dev->ring_heap);

   if (dev->default_heap)
      fd_bo_heap_destroy(dev->default_heap);

   fd_bo_cache_cleanup(&dev->bo_cache, 0);
   fd_bo_cache_cleanup(&dev->ring_cache, 0);

   /* Needs to be after bo cache cleanup in case backend has a
    * util_vma_heap that it destroys:
    */
   dev->funcs->destroy(dev);

   _mesa_hash_table_destroy(dev->handle_table, NULL);
   _mesa_hash_table_destroy(dev->name_table, NULL);

   if (fd_device_threaded_submit(dev))
      util_queue_destroy(&dev->submit_queue);

   if (dev->closefd)
      close(dev->fd);

   free(dev);
}

int
fd_device_fd(struct fd_device *dev)
{
   return dev->fd;
}

enum fd_version
fd_device_version(struct fd_device *dev)
{
   return dev->version;
}

void
fd_device_disable_explicit_sync_heuristic(struct fd_device *dev)
{
   dev->disable_explicit_sync_heuristic = true;
}

DEBUG_GET_ONCE_BOOL_OPTION(libgl, "LIBGL_DEBUG", false)

bool
fd_dbg(void)
{
   return debug_get_option_libgl();
}

uint32_t
fd_get_features(struct fd_device *dev)
{
   return dev->features;
}

bool
fd_has_syncobj(struct fd_device *dev)
{
#ifdef HAVE_LIBDRM
   uint64_t value;
   if (drmGetCap(dev->fd, DRM_CAP_SYNCOBJ, &value))
      return false;
   return value && dev->version >= FD_VERSION_FENCE_FD;
#else
   /* kgsl is not a DRM device and has no syncobj support */
   return false;
#endif
}
