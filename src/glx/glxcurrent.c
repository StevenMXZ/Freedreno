/*
 * SGI FREE SOFTWARE LICENSE B (Version 2.0, Sept. 18, 2008)
 * Copyright (C) 1991-2000 Silicon Graphics, Inc. All Rights Reserved.
 *
 * SPDX-License-Identifier: SGI-B-2.0
 */

/**
 * \file glxcurrent.c
 * Client-side GLX interface for current context management.
 */

#include <pthread.h>
#include <X11/Xatom.h>

#include "glxclient.h"
#include "glapi.h"
#include "glx_error.h"

/*
** We setup some dummy structures here so that the API can be used
** even if no context is current.
*/

static GLubyte dummyBuffer[__GLX_BUFFER_LIMIT_SIZE];
/*
** dummyVtable.copy_context and .swap_buffers are set so glXCopyContext / glXSwapBuffers still
** issue the corresponding X protocol request when invoked with no current context (the GLX spec
** does not require a current context for either call).
*/
static const struct glx_context_vtable dummyVtable = {
   .copy_context = __glXCopyContext,
   .swap_buffers = __glXSwapBuffers,
};
/*
** Dummy context used by small commands when there is no current context.
** All the
** gl and glx entry points are designed to operate as nop's when using
** the dummy context structure.
*/
struct glx_context dummyContext = {
   &dummyBuffer[0],
   &dummyBuffer[0],
   &dummyBuffer[0],
   &dummyBuffer[__GLX_BUFFER_LIMIT_SIZE],
   sizeof(dummyBuffer),
   &dummyVtable
};

/*
 * Current context management and locking
 */

pthread_mutex_t __glXmutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * Per-thread GLX context pointer.
 *
 * \c __glXSetCurrentContext is written is such a way that this pointer can
 * \b never be \c NULL.  This is important!  Because of this
 * \c __glXGetCurrentContext can be implemented as trivial macro.
 */
__THREAD_INITIAL_EXEC void *__glX_tls_Context = &dummyContext;

void
__glXSetCurrentContext(struct glx_context * c)
{
   __glX_tls_Context = (c != NULL) ? c : &dummyContext;
}

void
__glXSetCurrentContextNull(void)
{
   __glXSetCurrentContext(&dummyContext);
#if defined(GLX_DIRECT_RENDERING)
   _mesa_glapi_set_dispatch(NULL);   /* no-op functions */
   _mesa_glapi_set_context(NULL);
#endif
}

_GLX_PUBLIC GLXContext
glXGetCurrentContext(void)
{
   struct glx_context *cx = __glXGetCurrentContext();

   if (cx == &dummyContext) {
      return NULL;
   }
   else {
      return (GLXContext) cx;
   }
}

_GLX_PUBLIC GLXDrawable
glXGetCurrentDrawable(void)
{
   struct glx_context *gc = __glXGetCurrentContext();
   return gc->currentDrawable;
}

/**
 * Make a particular context current.
 *
 * \note This is in this file so that it can access dummyContext.
 */
static Bool
MakeContextCurrent(Display * dpy, GLXDrawable draw,
                   GLXDrawable read, GLXContext gc_user,
                   int opcode)
{
   struct glx_context *gc = (struct glx_context *) gc_user;
   struct glx_context *oldGC = __glXGetCurrentContext();
   int ret = GL_TRUE;

   /* Make sure that the new context has a nonzero ID.  In the request,
    * a zero context ID is used only to mean that we bind to no current
    * context.
    */
   if ((gc != NULL) && (gc->xid == None)) {
      return GL_FALSE;
   }

   /* can't have only one be 0 */
   if (!!draw != !!read) {
      __glXSendError(dpy, BadMatch, None, opcode, True);
      return False;
   }

   if (oldGC == gc &&
       gc->currentDrawable == draw && gc->currentReadable == read)
      return True;

   __glXLock();

   if (oldGC != &dummyContext) {
      oldGC->vtable->unbind(oldGC);
      oldGC->currentDpy = NULL;

      if (oldGC->xid == None) {
         /* We are switching away from a context that was
          * previously destroyed, so we need to free the memory
          * for the old handle. */
         oldGC->vtable->destroy(oldGC);
      }
   }

   __glXSetCurrentContextNull();

   if (gc) {
      /* GLX spec 3.3: If ctx is current to some other thread, then
       * glXMakeContextCurrent will generate a BadAccess error
       */
      if (gc->currentDpy)
      {
         __glXUnlock();
         __glXSendError(dpy, BadAccess, None, opcode, True);
         return False;
      }
      /* Attempt to bind the context.  We do this before mucking with
       * gc and __glXSetCurrentContext to properly handle our state in
       * case of an error.
       *
       * If an error occurs, set the Null context since we've already
       * blown away our old context.  The caller is responsible for
       * figuring out how to handle setting a valid context.
       */
      if (gc->vtable->bind(gc, draw, read) != Success) {
         ret = GL_FALSE;
      } else {
         gc->currentDpy = dpy;
         gc->currentDrawable = draw;
         gc->currentReadable = read;
         __glXSetCurrentContext(gc);
      }
   }

   __glXUnlock();

   if (!ret)
      __glXSendError(dpy, GLXBadContext, None, opcode, False);

   return ret;
}

/**
 * Advertise the driver on the drawable's window, which is how the Winlator
 * XServer is told to show its HUD: it reveals the overlay for the window that
 * gains a property whose name contains "_MESA_DRV", and reads
 * _MESA_DRV_ENGINE_NAME and _MESA_DRV_GPU_NAME for the two strings beside the
 * frame counter.  Without them the HUD never appears at all.
 *
 * The strings come from GLX_MESA_query_renderer rather than glGetString():
 * query_renderer.c is part of this library and asks the screen directly, while
 * the GL entrypoints are dispatch stubs whose resolution depends on how glapi
 * was linked.
 *
 * The property goes on the X window, not on the GLXDrawable -- this path
 * allocates a distinct GLXWindow XID, and XChangeProperty against that is a
 * BadWindow.
 */
static void
publish_driver_properties(Display *dpy, GLXDrawable drawable)
{
   /* Atoms are server-wide, so a name interns to the same value on every
    * connection and can be cached across displays.  XInternAtom is a round
    * trip and this runs per bind.
    */
   static Atom drv, engine_name, gpu_name;
   __GLXDRIdrawable *pdraw = GetGLXDRIDrawable(dpy, drawable);
   Window window = pdraw ? pdraw->xDrawable : (Window) drawable;
   const char *engine =
      glXQueryCurrentRendererStringMESA(GLX_RENDERER_VENDOR_ID_MESA);
   const char *gpu =
      glXQueryCurrentRendererStringMESA(GLX_RENDERER_DEVICE_ID_MESA);

   if (drv == None) {
      drv = XInternAtom(dpy, "_MESA_DRV", False);
      engine_name = XInternAtom(dpy, "_MESA_DRV_ENGINE_NAME", False);
      gpu_name = XInternAtom(dpy, "_MESA_DRV_GPU_NAME", False);
   }

#define SET_PROPERTY(atom, value)                                       \
   XChangeProperty(dpy, window, atom, XA_STRING, 8, PropModeReplace,    \
                   (const unsigned char *) (value), strlen(value))

   /* The trigger keys off the name alone, so the value carries nothing. */
   SET_PROPERTY(drv, "0");

   if (engine)
      SET_PROPERTY(engine_name, engine);
   if (gpu)
      SET_PROPERTY(gpu_name, gpu);

#undef SET_PROPERTY
}

_GLX_PUBLIC Bool
glXMakeCurrent(Display * dpy, GLXDrawable draw, GLXContext gc)
{
   Bool ret = MakeContextCurrent(dpy, draw, draw, gc, X_GLXMakeCurrent);

   /* Only once a context is really bound: unbinding passes None for both, and
    * the renderer strings have nothing to answer from without a current
    * context.  Repeating it on every bind would be three X requests for an
    * unchanged answer, and wined3d rebinds often, so remember the last one.
    */
   if (ret && gc != NULL && draw != None) {
      static __thread GLXDrawable published = None;

      if (published != draw) {
         published = draw;
         publish_driver_properties(dpy, draw);
      }
   }

   return ret;
}

_GLX_PUBLIC Bool
glXMakeContextCurrent(Display *dpy, GLXDrawable d, GLXDrawable r,
                      GLXContext ctx)
{
   return MakeContextCurrent(dpy, d, r, ctx, X_GLXMakeContextCurrent);
}

_GLX_PUBLIC Bool
glXMakeCurrentReadSGI(Display *dpy, GLXDrawable d, GLXDrawable r,
                      GLXContext ctx)
{
   return MakeContextCurrent(dpy, d, r, ctx, X_GLXvop_MakeCurrentReadSGI);
}
