/*
 * Copyright © 2013 Keith Packard
 * Copyright © 2015 Boyan Ding
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "x11_dri3.h"

#include <stdbool.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>

#include "util/log.h"
#include "util/os_misc.h"

/** x11_dri3_no_device_fd
 *
 * Answer for a server that implements DRI3 but has no DRM device to hand out.
 * Winlator's X server is one: it shares pixmaps and presents them, but its
 * Open reply carries no fd at all.
 *
 * The fd only names the device the driver will run on, so its absence is not
 * fatal as long as the driver can find that device itself. A pinned
 * MESA_LOADER_DRIVER_OVERRIDE is that promise, and is why this is gated on one:
 * without it loader_get_driver_for_fd() would probe the placeholder and report
 * "No driver found" instead of the caller's clearer diagnostic.
 *
 * /dev/null serves: the fd only has to survive being dup'ed and passed down.
 * loader_get_pci_id_for_fd() and drmGetVersion() both fail on it, landing on
 * PIPE_LOADER_DEVICE_PLATFORM.
 */
static int
x11_dri3_no_device_fd(void)
{
   const char *driver = os_get_option("MESA_LOADER_DRIVER_OVERRIDE");
   int fd;

   if (!driver || !*driver)
      return -1;

   fd = open("/dev/null", O_RDWR | O_CLOEXEC);
   if (fd < 0)
      return -1;

   mesa_logi("DRI3 Open returned no device fd; running %s against a "
             "placeholder, the driver has to find its device itself", driver);

   return fd;
}

/** x11_dri3_open
 *
 * Wrapper around xcb_dri3_open
 */
int
x11_dri3_open(xcb_connection_t *conn,
                 xcb_window_t root,
                 uint32_t provider)
{
   xcb_dri3_open_cookie_t       cookie;
   xcb_dri3_open_reply_t        *reply;
   int                          fd;
   const xcb_query_extension_reply_t *extension;

   xcb_prefetch_extension_data(conn, &xcb_dri3_id);
   extension = xcb_get_extension_data(conn, &xcb_dri3_id);
   if (!(extension && extension->present))
      return -1;

   cookie = xcb_dri3_open(conn,
                          root,
                          provider);

   reply = xcb_dri3_open_reply(conn, cookie, NULL);

   if (!reply)
      return -1;

   if (reply->nfd != 1) {
      free(reply);
      return x11_dri3_no_device_fd();
   }

   fd = xcb_dri3_open_reply_fds(conn, reply)[0];
   free(reply);
   fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);

   /* The loader only ever used XFixes to hand Present a damage region, and it
    * no longer builds one: an update region of None covers the whole window.
    * So there is nothing left to negotiate an XFixes version for, and a server
    * without the extension is no longer a reason to refuse DRI3.
    */

   return fd;
}

/* Only request versions of these protocols which we actually support. */
#define DRI3_SUPPORTED_MAJOR 1
#define PRESENT_SUPPORTED_MAJOR 1

#ifdef HAVE_X11_DRM
#define DRI3_SUPPORTED_MINOR 2
#define PRESENT_SUPPORTED_MINOR 2
#else
#define PRESENT_SUPPORTED_MINOR 0
#define DRI3_SUPPORTED_MINOR 0
#endif

bool
x11_dri3_has_multibuffer(xcb_connection_t *c)
{
   xcb_dri3_query_version_cookie_t      dri3_cookie;
   xcb_dri3_query_version_reply_t       *dri3_reply;
   xcb_present_query_version_cookie_t   present_cookie;
   xcb_present_query_version_reply_t    *present_reply;
   xcb_generic_error_t                  *error;
   const xcb_query_extension_reply_t    *extension;

   xcb_prefetch_extension_data(c, &xcb_dri3_id);
   xcb_prefetch_extension_data(c, &xcb_present_id);

   extension = xcb_get_extension_data(c, &xcb_dri3_id);
   if (!(extension && extension->present))
      return false;

   extension = xcb_get_extension_data(c, &xcb_present_id);
   if (!(extension && extension->present))
      return false;

   dri3_cookie = xcb_dri3_query_version(c,
                                        DRI3_SUPPORTED_MAJOR,
                                        DRI3_SUPPORTED_MINOR);
   present_cookie = xcb_present_query_version(c,
                                              PRESENT_SUPPORTED_MAJOR,
                                              PRESENT_SUPPORTED_MINOR);

   dri3_reply = xcb_dri3_query_version_reply(c, dri3_cookie, &error);
   if (!dri3_reply) {
      free(error);
      return false;
   }

   int dri3Major = dri3_reply->major_version;
   int dri3Minor = dri3_reply->minor_version;
   free(dri3_reply);

   present_reply = xcb_present_query_version_reply(c, present_cookie, &error);
   if (!present_reply) {
      free(error);
      return false;
   }
   int presentMajor = present_reply->major_version;
   int presentMinor = present_reply->minor_version;
   free(present_reply);

   return (presentMajor > 1 || (presentMajor == 1 && presentMinor >= 2)) &&
      (dri3Major > 1 || (dri3Major == 1 && dri3Minor >= 2));
}
