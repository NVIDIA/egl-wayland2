/*
 * SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <assert.h>

#include <xf86drm.h>
#include <gbm.h>

#include <GL/gl.h>

#include <wayland-egl-backend.h>
#include <wayland-client-protocol.h>

#include "wayland-platform.h"
#include "wayland-display.h"
#include "wayland-swapchain.h"
#include "wayland-dmabuf.h"

static const int WL_EGL_WINDOW_DESTROY_CALLBACK_SINCE = 3;

struct _EplImplSurface
{
    /// A pointer back to the owning display.
    WlDisplayInstance *inst;

    long int native_window_version;
    struct wl_surface *wsurf;

    /**
     * The color format that we're using for this window.
     *
     * This is an entry in the driver's format list.
     */
    const WlDmaBufFormat *driver_format;

    /**
     * Contains data that should only be accessed while the surface is current
     * or destroyed.
     *
     * Since everything in this struct can only ever be accessed by one thread
     * at a time, we don't need a mutex for it.
     */
    struct
    {
        struct wl_event_queue *queue;

        /**
         * The explicit synchronization object for this surface, or NULL if we
         * aren't using explicit sync.
         */
        struct wp_linux_drm_syncobj_surface_v1 *syncobj;

        /**
         * The current swapchain for this surface.
         */
        WlSwapChain *swapchain;

        /**
         * A callback for a pending wl_frame event.
         */
        struct wl_callback *frame_callback;

        /**
         * The set of modifiers that we should try to use for this surface.
         *
         * If we're always using PRIME (i.e., we're rendering to a different
         * device than the server's main device), then this will be empty. In
         * that case, the present buffers will always be linear, and the render
         * buffers only need to care about the driver's supported modifiers.
         */
        uint64_t *surface_modifiers;
        size_t num_surface_modifiers;

        /**
         * If true, then we should try to reallocate the swapchain even if
         * nothing appears to have changed.
         *
         * If eglPlatformSetColorBuffersNVX failed because it couldn't allocate
         * the ancillary buffers, then it may have to leave a dummy surface in
         * place. In that case, we'll need to reallocate the swapchain in order
         * to actually render anything.
         */
        EGLBoolean force_realloc;
    } current;

    /**
     * Contains surface parameters which can be modified by any thread.
     *
     * We have to hold a mutex while accessing anything in this struct, but we
     * must NOT call into the driver while holding the mutex.
     */
    struct
    {
        pthread_mutex_t mutex;

        struct wl_egl_window *native_window;

        /**
         * The current swap interval, as set by eglSwapInterval.
         */
        EGLint swap_interval;

        /**
         * If this is non-zero, then ignore the update callback.
         *
         * This is used in eglSwapBuffers and during teardown.
         */
        unsigned int skip_update_callback;

        /**
         * The pending width and height is set in response to a window resize.
         *
         * If the pending size is different than the current size, then that means
         * we need to reallocate the shared color buffers for this window.
         */
        EGLint pending_width;
        EGLint pending_height;
    } params;
};


/**
 * Sets the surface's modifier list to use the modifiers from the default
 * dma-buf feedback.
 *
 * This is used as a fallback if we don't have per-surface feedback.
 */
static void PickDefaultModifiers(EplSurface *psurf)
{
    const WlDmaBufFormat *driver_format = psurf->priv->driver_format;
    const WlDmaBufFormat *server_format = eplWlDmaBufFormatFind(psurf->priv->inst->default_feedback->formats,
            psurf->priv->inst->default_feedback->num_formats, driver_format->fourcc);
    size_t i;

    psurf->priv->current.num_surface_modifiers = 0;

    if (server_format == NULL)
    {
        // This should never happen: If we didn't find server support for this
        // format, then we should never have set EGL_WINDOW_BIT for the
        // EGLConfig.
        assert(server_format != NULL);
        return;
    }

    for (i=0; i<driver_format->num_modifiers; i++)
    {
        if (eplWlDmaBufFormatSupportsModifier(server_format, driver_format->modifiers[i]))
        {
            psurf->priv->current.surface_modifiers[psurf->priv->current.num_surface_modifiers++] = driver_format->modifiers[i];
        }
    }

    // Same as above: If the server doesn't support linear or some common
    // format with the driver, then we shouldn't have set EGL_WINDOW_BIT.
    assert(psurf->priv->current.num_surface_modifiers > 0
            || eplWlDmaBufFormatSupportsModifier(server_format, DRM_FORMAT_MOD_LINEAR));
}

/**
 * Checks if we need to allocate a new swapchain.
 *
 * If the current swapchain is not NULL, then this function will check to see
 * if it's still valid to use. If it is, then this function will return
 * EGL_TRUE, and will return NULL in \p ret_new_swapchain.
 *
 * \note This function must only be called during surface creation, or while
 * the surface is current.
 *
 * \param psurf The surface that we're allocating a swapchain for.
 * \param allow_modifier_realloc If true, then create a new swapchain with
 *      different format modifiers, even if the size hasn't changed.
 * \param[out] ret_new_swapchain Returns the new swapchain, or NULL if the
 *      current swapchain should still be used.
 *
 * \return EGL_TRUE on success, or EGL_FALSE on error.
 */
static EGLBoolean SwapChainRealloc(EplSurface *psurf,
        EGLBoolean allow_modifier_realloc, WlSwapChain **ret_new_swapchain)
{
    const WlDmaBufFormat *driver_format = psurf->priv->driver_format;
    WlSwapChain *swapchain = NULL;
    uint32_t width, height;
    EGLBoolean needs_new = EGL_FALSE;
    EGLBoolean success = EGL_FALSE;

    pthread_mutex_lock(&psurf->priv->params.mutex);
    width = psurf->priv->params.pending_width;
    height = psurf->priv->params.pending_height;
    pthread_mutex_unlock(&psurf->priv->params.mutex);

    if (psurf->priv->current.swapchain == NULL || psurf->priv->current.force_realloc)
    {
        needs_new = EGL_TRUE;
    }
    else if (width != psurf->priv->current.swapchain->width
            || height != psurf->priv->current.swapchain->height)
    {
        needs_new = EGL_TRUE;
    }

    if (needs_new)
    {
        // We don't support PRIME yet, so we should always have a set of
        // possible modifiers.
        assert(psurf->priv->current.num_surface_modifiers > 0);
        swapchain = eplWlSwapChainCreate(psurf->priv->inst, psurf->priv->current.queue,
                width, height, driver_format->fourcc, EGL_FALSE,
                psurf->priv->current.surface_modifiers,
                psurf->priv->current.num_surface_modifiers);
        if (swapchain == NULL)
        {
            goto done;
        }
    }

    success = EGL_TRUE;

done:
    *ret_new_swapchain = swapchain;
    return success;
}

static EGLBoolean IsMemoryReadable(const void *p, size_t len)
{
    int fds[2], result = -1;

    /*
     * If the address is below some small-ish value, then assume it's not
     * readable. This is mainly useful as an early-out when we're trying to
     * figure out if a wl_egl_window starts with a version number or a
     * wl_surface.
     */
    if (((uintptr_t) p) < 256)
    {
        return EGL_FALSE;
    }

    if (pipe(fds) == -1) {
        return EGL_FALSE;
    }

    if (fcntl(fds[1], F_SETFL, O_NONBLOCK) == -1) {
        goto done;
    }

    /* write will fail with EFAULT if the provided buffer is outside
     * our accessible address space. */
    result = write(fds[1], p, len);
    assert(result != -1 || errno == EFAULT);

done:
    close(fds[0]);
    close(fds[1]);
    return result != -1;
}

static EGLBoolean CheckInterfaceType(struct wl_object *obj, const char *ifname)
{
    /* The first member of a wl_object is a pointer to its wl_interface, */
    struct wl_interface *interface = *(void **)obj;

    /* Check if the memory for the wl_interface struct, and the
     * interface name, are safe to read. */
    int len = strlen(ifname);
    if (!IsMemoryReadable(interface, sizeof (*interface))
            || !IsMemoryReadable(interface->name, len + 1))
    {
        return EGL_FALSE;
    }

    return !strcmp(interface->name, ifname);
}

/**
 * Returns the version number and the wl_surface pointer from a wl_egl_window.
 */
static EGLBoolean GetWlEglWindowVersionAndSurface(struct wl_egl_window *window,
        long int *ret_version, struct wl_surface **ret_surface)
{
    long int version = 0;
    struct wl_surface *surface = NULL;

    if (window == NULL || !IsMemoryReadable(window, sizeof (*window)))
    {
        return EGL_FALSE;
    }

    /*
     * Given that wl_egl_window wasn't always a versioned struct, and that
     * 'window->version' replaced 'window->surface', we must check whether
     * 'window->version' is actually a valid pointer. If it is, we are dealing
     * with a wl_egl_window from an old implementation of libwayland-egl.so
     */

    if (IsMemoryReadable((void *)window->version, sizeof (void *)))
    {
        version = 0;
        surface = (struct wl_surface *) window->version;
    }
    else
    {
        version = window->version;
        surface = window->surface;
    }

    if (!CheckInterfaceType((struct wl_object *)surface, "wl_surface"))
    {
        return EGL_FALSE;
    }

    *ret_version = version;
    *ret_surface = surface;
    return EGL_TRUE;
}

static void NativeResizeCallback(struct wl_egl_window *native, void *param)
{
    EplSurface *psurf = param;
    if (psurf == NULL || psurf->priv == NULL)
    {
        return;
    }

    if (native->width > 0 && native->height > 0)
    {
        pthread_mutex_lock(&psurf->priv->params.mutex);
        psurf->priv->params.pending_width = native->width;
        psurf->priv->params.pending_height = native->height;
        pthread_mutex_unlock(&psurf->priv->params.mutex);
    }
}

static void NativeDestroyWindowCallback(void *param)
{
    EplSurface *psurf = param;
    if (psurf == NULL || psurf->priv == NULL)
    {
        return;
    }

    pthread_mutex_lock(&psurf->priv->params.mutex);
    psurf->priv->params.native_window = NULL;
    pthread_mutex_unlock(&psurf->priv->params.mutex);
}

static void SetWindowSwapchain(EplSurface *psurf, WlSwapChain *swapchain)
{
    EGLAttrib buffers[] =
    {
        GL_BACK, (EGLAttrib) swapchain->render_buffer,
        EGL_NONE
    };
    if (psurf->priv->inst->platform->priv->egl.PlatformSetColorBuffersNVX(
                psurf->priv->inst->internal_display->edpy,
                psurf->internal_surface, buffers))
    {
        eplWlSwapChainDestroy(psurf->priv->inst, psurf->priv->current.swapchain);
        psurf->priv->current.swapchain = swapchain;
        psurf->priv->current.force_realloc = EGL_FALSE;
    }
    else
    {
        // Free the new swapchain. We'll try again next time.
        eplWlSwapChainDestroy(psurf->priv->inst, swapchain);
        psurf->priv->current.force_realloc = EGL_TRUE;
    }
}

static void WindowUpdateCallback(void *param)
{
    EplSurface *psurf = param;
    WlSwapChain *swapchain = NULL;

    pthread_mutex_lock(&psurf->priv->params.mutex);
    if (psurf->priv->params.skip_update_callback != 0
            || psurf->priv->params.native_window == NULL)
    {
        pthread_mutex_unlock(&psurf->priv->params.mutex);
        return;
    }
    pthread_mutex_unlock(&psurf->priv->params.mutex);

    if (!SwapChainRealloc(psurf, EGL_FALSE, &swapchain))
    {
        return;
    }

    if (swapchain != NULL)
    {
        SetWindowSwapchain(psurf, swapchain);
    }
}

EGLSurface eplWlCreateWindowSurface(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *psurf,
        EGLConfig config, void *native_surface, const EGLAttrib *attribs, EGLBoolean create_platform,
        const struct glvnd_list *existing_surfaces)
{
    const EplSurface *otherSurf = NULL;
    WlDisplayInstance *inst = pdpy->priv->inst;
    EplImplSurface *priv = NULL;
    struct wl_egl_window *window = native_surface;
    long int windowVersion = 0;
    struct wl_surface *wsurf = NULL;
    const EplConfig *configInfo = NULL;
    const WlDmaBufFormat *driver_format = NULL;
    EGLSurface internalSurface = EGL_NO_SURFACE;
    EGLAttrib platformAttribs[] =
    {
        GL_BACK, 0,
        EGL_PLATFORM_SURFACE_UPDATE_CALLBACK_NVX, (EGLAttrib) WindowUpdateCallback,
        EGL_PLATFORM_SURFACE_UPDATE_CALLBACK_PARAM_NVX, (EGLAttrib) psurf,
        EGL_NONE
    };

    if (!GetWlEglWindowVersionAndSurface(window, &windowVersion, &wsurf))
    {
        eplSetError(plat, EGL_BAD_NATIVE_WINDOW, "wl_egl_window %p is invalid", window);
        return EGL_NO_SURFACE;
    }

    /*
     * Make sure that there isn't already an EGLSurface for this wl_surface.
     *
     * Note that we can't just check the wl_egl_window pointer itself, because
     * an application can call wl_egl_window_create multiple times to create
     * multiple wl_egl_window structs for the same wl_surface.
     */
    glvnd_list_for_each_entry(otherSurf, existing_surfaces, entry)
    {
        if (otherSurf->type == EPL_SURFACE_TYPE_WINDOW && otherSurf->priv != NULL && otherSurf->priv->wsurf == wsurf)
        {
            eplSetError(pdpy->platform, EGL_BAD_ALLOC,
                    "An EGLSurface already exists for wl_surface %p\n", wsurf);
            return EGL_FALSE;
        }
    }

    configInfo = eplConfigListFind(inst->configs, config);
    if (configInfo == NULL)
    {
        eplSetError(plat, EGL_BAD_CONFIG, "Invalid EGLConfig %p", config);
        return EGL_NO_SURFACE;
    }
    if (!(configInfo->surfaceMask & EGL_WINDOW_BIT))
    {
        eplSetError(plat, EGL_BAD_CONFIG, "EGLConfig %p does not support windows", config);
        return EGL_NO_SURFACE;
    }

    driver_format = eplWlDmaBufFormatFind(inst->driver_formats->formats, inst->driver_formats->num_formats, configInfo->fourcc);
    assert(driver_format != NULL);

    // Allocate enough space for the EplImplSurface, plus extra to hold a
    // format modifier list.
    priv = calloc(1, sizeof(EplImplSurface)
            + driver_format->num_modifiers * sizeof(uint64_t));
    if (priv == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Out of memory");
        return EGL_NO_SURFACE;
    }

    if (pthread_mutex_init(&priv->params.mutex, NULL) != 0)
    {
        free(priv);
        eplSetError(plat, EGL_BAD_ALLOC, "Failed to create internal mutex");
        return EGL_NO_SURFACE;
    }

    psurf->priv = priv;
    priv->current.surface_modifiers = (uint64_t *) (priv + 1);
    priv->inst = eplWlDisplayInstanceRef(inst);
    priv->wsurf = wsurf;
    priv->native_window_version = windowVersion;
    priv->driver_format = driver_format;

    priv->params.native_window = window;
    priv->params.swap_interval = 1;
    priv->params.pending_width = (window->width > 0 ? window->width : 1);
    priv->params.pending_height = (window->height > 0 ? window->height : 1);
    priv->current.queue = wl_display_create_queue(inst->wdpy);
    if (priv->current.queue == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Failed to create internal event queue");
        goto done;
    }

    if (inst->globals.syncobj != NULL)
    {
        priv->current.syncobj = wp_linux_drm_syncobj_manager_v1_get_surface(inst->globals.syncobj, wsurf);
        if (priv->current.syncobj == NULL)
        {
            goto done;
        }
    }

    // Initialize the modifier list based on the default modifiers.
    PickDefaultModifiers(psurf);

    // TODO: If we've got a new enough version of linux-dmabuf-v1, then set up
    // a surface feedback object here and let that override the default
    // modifiers.

    // Now that we've got our format modifier list, allocate the initial
    // swapchain.
    if (!SwapChainRealloc(psurf, EGL_FALSE, &priv->current.swapchain))
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Failed to create color buffers");
        goto done;
    }
    assert(priv->current.swapchain != NULL);

    /*
     * Note that we don't need any internal attributes here. The
     * linux-dmabuf-v1 protocol has a flag for whether a buffer is y-inverted
     * or not.
     */

    platformAttribs[1] = (EGLAttrib) priv->current.swapchain->render_buffer;
    internalSurface = inst->platform->priv->egl.PlatformCreateSurfaceNVX(inst->internal_display->edpy,
            config, platformAttribs, attribs);
    if (internalSurface == EGL_NO_SURFACE)
    {
        goto done;
    }

    window->driver_private = psurf;
    window->resize_callback = NativeResizeCallback;
    if (windowVersion >= WL_EGL_WINDOW_DESTROY_CALLBACK_SINCE)
    {
        window->destroy_window_callback = NativeDestroyWindowCallback;
    }

done:
    if (internalSurface == EGL_NO_SURFACE)
    {
        eplWlDestroyWindow(pdpy, psurf, existing_surfaces);
    }
    return internalSurface;
}

void eplWlDestroyWindow(EplDisplay *pdpy, EplSurface *psurf,
            const struct glvnd_list *existing_surfaces)
{
    if (psurf->priv == NULL)
    {
        assert(psurf->internal_surface == EGL_NO_SURFACE);
        return;
    }
    assert(psurf->type == EPL_SURFACE_TYPE_WINDOW);

    if (psurf->internal_surface != EGL_NO_SURFACE)
    {
        /*
         * Increment the skip counter, and then destroy the internal surface.
         *
         * If the surface is still current to another thread, then the driver will
         * ensure that any callbacks have finished, and that no new callbacks will
         * start.
         */
        pthread_mutex_lock(&psurf->priv->params.mutex);
        psurf->priv->params.skip_update_callback++;
        pthread_mutex_unlock(&psurf->priv->params.mutex);

        psurf->priv->inst->platform->egl.DestroySurface(psurf->priv->inst->internal_display->edpy, psurf->internal_surface);
        psurf->internal_surface = EGL_NO_SURFACE;
    }

    if (psurf->priv->params.native_window != NULL)
    {
        psurf->priv->params.native_window->resize_callback = NULL;
        if (psurf->priv->native_window_version >= WL_EGL_WINDOW_DESTROY_CALLBACK_SINCE)
        {
            psurf->priv->params.native_window->destroy_window_callback = NULL;
        }
        psurf->priv->params.native_window->driver_private = NULL;
    }

    if (psurf->priv->current.swapchain != NULL)
    {
        eplWlSwapChainDestroy(psurf->priv->inst, psurf->priv->current.swapchain);
    }
    if (psurf->priv->current.syncobj != NULL)
    {
        wp_linux_drm_syncobj_surface_v1_destroy(psurf->priv->current.syncobj);
    }
    if (psurf->priv->current.frame_callback != NULL)
    {
        wl_callback_destroy(psurf->priv->current.frame_callback);
    }
    if (psurf->priv->current.queue != NULL)
    {
        wl_event_queue_destroy(psurf->priv->current.queue);
    }

    pthread_mutex_destroy(&psurf->priv->params.mutex);

    free(psurf->priv);
    eplWlDisplayInstanceUnref(psurf->priv->inst);
    psurf->priv = NULL;
}

EGLBoolean eplWlSwapBuffers(EplPlatformData *plat, EplDisplay *pdpy,
        EplSurface *psurf, const EGLint *rects, EGLint n_rects)
{
    // Not implemented yet.
    return EGL_FALSE;
}
