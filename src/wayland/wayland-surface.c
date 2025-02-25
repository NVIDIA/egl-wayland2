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

#include <stdio.h>
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

/**
 * Keeps track of a per-surface dma-buf feedback object.
 *
 * This is currently only used if we're rendering to the server's main device.
 * If we're not using the main device, then we have to use the PRIME path
 * anyway, which means the wl_buffers will always be linear.
 */
typedef struct
{
    WlDmaBufFeedbackCommon base;

    EplSurface *psurf;

    struct zwp_linux_dmabuf_feedback_v1 *feedback;

    /**
     * The set of modifiers that the server supports.
     *
     * This array is parallel to the driver format modifier list for the
     * surface. A value of EGL_TRUE indicates that the corresponding modifier
     * is supported.
     *
     * This is copied from \c tranche_modifiers_supported when we get a
     * zwp_linux_dmabuf_feedback_v1::tranche_done event.
     */
    EGLBoolean *modifiers_supported;

    /// True if the server supports a linear buffer.
    EGLBoolean linear_supported;

    /**
     * The supported modifiers in the current tranche.
     */
    EGLBoolean *tranche_modifiers_supported;

    EGLBoolean tranche_linear_supported;

    /**
     * True if we've received new feedback data.
     */
    EGLBoolean modifiers_changed;
} SurfaceFeedbackState;

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
         * A dma-buf feedback object for this surface.
         */
        SurfaceFeedbackState *feedback;

        /**
         * The set of modifiers that we should try to use for this surface.
         *
         * This is set based on either the default feedback or a per-surface
         * zwp_linux_dmabuf_feedback_v1 object.
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
    const WlDmaBufFormat *server_format;
    size_t i;

    psurf->priv->current.num_surface_modifiers = 0;

    if (psurf->priv->inst->force_prime)
    {
        // If we have to use PRIME, then leave the modifier list empty. The
        // present buffers will all be linear, and the render buffer only has
        // to match the driver, not the server's support.
        return;
    }

    server_format = eplWlDmaBufFormatFind(psurf->priv->inst->default_feedback->formats,
            psurf->priv->inst->default_feedback->num_formats, driver_format->fourcc);

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
 * Returns true if we've already found the next set of modifiers that we're
 * going to use for buffer allocation, and so we should ignore any other
 * tranches.
 */
static EGLBoolean SurfaceFeedbackHasModifiers(SurfaceFeedbackState *state)
{
    size_t i;

    for (i=0; i<state->psurf->priv->driver_format->num_modifiers; i++)
    {
        if (state->modifiers_supported[i] || state->linear_supported)
        {
            return EGL_TRUE;
        }
    }
    return EGL_FALSE;
}

static void OnSurfaceFeedbackTrancheFormats(void *userdata,
            struct zwp_linux_dmabuf_feedback_v1 *wfeedback,
            struct wl_array *indices)
{
    SurfaceFeedbackState *state = userdata;
    EplSurface *psurf = state->psurf;
    size_t i;
    uint16_t *index;

    if (state->base.error || state->base.format_table_len == 0 || SurfaceFeedbackHasModifiers(state))
    {
        return;
    }

    wl_array_for_each(index, indices)
    {
        if (*index >= state->base.format_table_len)
        {
            continue;
        }
        if (state->base.format_table[*index].fourcc != psurf->priv->driver_format->fourcc)
        {
            continue;
        }
        if (state->base.format_table[*index].modifier == DRM_FORMAT_MOD_LINEAR)
        {
            state->tranche_linear_supported = EGL_TRUE;
        }
        else
        {
            for (i=0; i<psurf->priv->driver_format->num_modifiers; i++)
            {
                if (psurf->priv->driver_format->modifiers[i] == state->base.format_table[*index].modifier)
                {
                    state->tranche_modifiers_supported[i] = EGL_TRUE;
                    break;
                }
            }
        }
    }
}

static void OnSurfaceFeedbackTrancheDone(void *userdata,
        struct zwp_linux_dmabuf_feedback_v1 *wfeedback)
{
    SurfaceFeedbackState *state = userdata;
    EplSurface *psurf = state->psurf;
    EGLBoolean use_tranche = EGL_FALSE;
    size_t i;

    if (!state->base.error && !SurfaceFeedbackHasModifiers(state))
    {
        for (i=0; i<psurf->priv->inst->render_device_id_count; i++)
        {
            if (state->base.tranche_target_device != psurf->priv->inst->render_device_id[i])
            {
                use_tranche = EGL_TRUE;
                break;
            }
        }
    }

    if (use_tranche)
    {
        for (i=0; i<psurf->priv->driver_format->num_modifiers; i++)
        {
            state->modifiers_supported[i] = state->tranche_modifiers_supported[i];
            state->tranche_modifiers_supported[i] = EGL_FALSE;
        }
        state->linear_supported = state->tranche_linear_supported;
        state->tranche_linear_supported = EGL_FALSE;
    }

    eplWlDmaBufFeedbackCommonTrancheDone(&state->base);
}

static void OnSurfaceFeedbackDone(void *userdata,
        struct zwp_linux_dmabuf_feedback_v1 *wfeedback)
{
    SurfaceFeedbackState *state = userdata;
    EplSurface *psurf = state->psurf;
    size_t i;

    psurf->priv->current.num_surface_modifiers = 0;
    for (i=0; i<psurf->priv->driver_format->num_modifiers; i++)
    {
        if (state->modifiers_supported[i])
        {
            psurf->priv->current.surface_modifiers[psurf->priv->current.num_surface_modifiers++]
                = psurf->priv->driver_format->modifiers[i];
        }

        // Clear the modifier arrays to get ready for the next update.
        state->modifiers_supported[i] = EGL_FALSE;
        state->tranche_modifiers_supported[i] = EGL_FALSE;
    }

    if (psurf->priv->current.num_surface_modifiers == 0)
    {
        /*
         * The server didn't advertise any modifiers that we support.
         *
         * We only use surface feedback if we're rendering on the server's main
         * device, so if the server advertises linear, then that probably means
         * the window is being displayed on another (non-main) device that can
         * scan out from a linear buffer. In that case, we'll use PRIME.
         *
         * Otherwise, fall back to using the default feedback data so that we
         * at least have something that the server can read.
         */
        if (!state->linear_supported)
        {
            PickDefaultModifiers(psurf);
        }
    }

    state->linear_supported = EGL_FALSE;
    state->tranche_linear_supported = EGL_FALSE;
    state->modifiers_changed = EGL_TRUE;
    eplWlDmaBufFeedbackCommonDone(&state->base);
}

static const struct zwp_linux_dmabuf_feedback_v1_listener SURFACE_FEEDBACK_LISTENER =
{
    OnSurfaceFeedbackDone,
    eplWlDmaBufFeedbackCommonFormatTable,
    eplWlDmaBufFeedbackCommonMainDevice,
    OnSurfaceFeedbackTrancheDone,
    eplWlDmaBufFeedbackCommonTrancheTargetDevice,
    OnSurfaceFeedbackTrancheFormats,
    eplWlDmaBufFeedbackCommonTrancheFlags,
};

static EGLBoolean CreateSurfaceFeedback(EplSurface *psurf)
{
    WlDisplayInstance *inst = psurf->priv->inst;
    SurfaceFeedbackState *state;
    struct zwp_linux_dmabuf_v1 *wrapper = NULL;

    if (inst->force_prime || wl_proxy_get_version((struct wl_proxy *) inst->globals.dmabuf) < 4)
    {
        return EGL_TRUE;
    }

    state = calloc(1, sizeof(SurfaceFeedbackState)
            + psurf->priv->driver_format->num_modifiers * (2 * sizeof(EGLBoolean)));
    if (state == NULL)
    {
        eplSetError(inst->platform, EGL_BAD_ALLOC, "Out of memory");
        return EGL_FALSE;
    }

    eplWlDmaBufFeedbackCommonInit(&state->base);
    state->psurf = psurf;
    state->modifiers_supported = (EGLBoolean *) (state + 1);
    state->tranche_modifiers_supported = (EGLBoolean *)
        (state->modifiers_supported + psurf->priv->driver_format->num_modifiers);
    psurf->priv->current.feedback = state;

    wrapper = wl_proxy_create_wrapper(inst->globals.dmabuf);
    if (wrapper == NULL)
    {
        eplSetError(inst->platform, EGL_BAD_ALLOC, "Out of memory");
        return EGL_FALSE;
    }

    wl_proxy_set_queue((struct wl_proxy *) wrapper, psurf->priv->current.queue);
    state->feedback = zwp_linux_dmabuf_v1_get_surface_feedback(wrapper, psurf->priv->wsurf);
    wl_proxy_wrapper_destroy(wrapper);

    if (state->feedback == NULL)
    {
        eplSetError(inst->platform, EGL_BAD_ALLOC, "Out of memory");
        return EGL_FALSE;
    }

    zwp_linux_dmabuf_feedback_v1_add_listener(state->feedback, &SURFACE_FEEDBACK_LISTENER, state);

    // Do a single round trip. The server should send a full batch of feedback
    // data, but if it doesn't, then the modifier list is already initialized
    // using the default feedback.
    if (wl_display_roundtrip_queue(inst->wdpy, psurf->priv->current.queue) < 0)
    {
        eplSetError(inst->platform, EGL_BAD_ALLOC, "Failed to read window system events");
        return EGL_FALSE;
    }

    return EGL_TRUE;
}

static void DestroySurfaceFeedback(EplSurface *psurf)
{
    if (psurf->priv->current.feedback != NULL)
    {
        if (psurf->priv->current.feedback->feedback != NULL)
        {
            zwp_linux_dmabuf_feedback_v1_destroy(psurf->priv->current.feedback->feedback);
        }
        eplWlDmaBufFeedbackCommonCleanup(&psurf->priv->current.feedback->base);
        free(psurf->priv->current.feedback);
        psurf->priv->current.feedback = NULL;
    }
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
    else if (allow_modifier_realloc
            && psurf->priv->current.feedback != NULL
            && psurf->priv->current.feedback->modifiers_changed)
    {
        if (psurf->priv->current.swapchain->prime)
        {
            if (psurf->priv->current.num_surface_modifiers > 0)
            {
                // Transition from prime to direct
                needs_new = EGL_TRUE;
            }
        }
        else
        {
            size_t i;
            needs_new = EGL_TRUE;
            for (i=0; i<psurf->priv->current.num_surface_modifiers; i++)
            {
                if (psurf->priv->current.swapchain->modifier == psurf->priv->current.surface_modifiers[i])
                {
                    // Transition from direct to either prime or direct with different modifiers
                    needs_new = EGL_FALSE;
                    break;
                }
            }
        }
    }

    if (needs_new)
    {
        if (psurf->priv->current.num_surface_modifiers > 0)
        {
            swapchain = eplWlSwapChainCreate(psurf->priv->inst, psurf->priv->current.queue,
                    width, height, driver_format->fourcc, EGL_FALSE,
                    psurf->priv->current.surface_modifiers,
                    psurf->priv->current.num_surface_modifiers);
        }
        else
        {
            swapchain = eplWlSwapChainCreate(psurf->priv->inst, psurf->priv->current.queue,
                    width, height, driver_format->fourcc, EGL_TRUE,
                    driver_format->modifiers, driver_format->num_modifiers);
        }
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

    if (plat->priv->wl.display_create_queue_with_name != NULL)
    {
        char name[64];
        snprintf(name, sizeof(name), "EGLSurface(%u)", wl_proxy_get_id((struct wl_proxy *) wsurf));
        priv->current.queue = plat->priv->wl.display_create_queue_with_name(inst->wdpy, name);
    }
    else
    {
        priv->current.queue = wl_display_create_queue(inst->wdpy);
        abort();
    }
    if (priv->current.queue == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Failed to create internal event queue");
        goto done;
    }

    priv->wsurf = wl_proxy_create_wrapper(wsurf);
    if (priv->wsurf == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Failed to create internal wl_surface wrapper");
        goto done;
    }
    wl_proxy_set_queue((struct wl_proxy *) priv->wsurf, priv->current.queue);

    priv->native_window_version = windowVersion;
    priv->driver_format = driver_format;

    priv->params.native_window = window;
    priv->params.swap_interval = 1;
    priv->params.pending_width = (window->width > 0 ? window->width : 1);
    priv->params.pending_height = (window->height > 0 ? window->height : 1);

    if (inst->globals.syncobj != NULL)
    {
        priv->current.syncobj = wp_linux_drm_syncobj_manager_v1_get_surface(inst->globals.syncobj, priv->wsurf);
        if (priv->current.syncobj == NULL)
        {
            goto done;
        }
    }

    // Initialize the modifier list based on the default modifiers.
    PickDefaultModifiers(psurf);

    if (!CreateSurfaceFeedback(psurf))
    {
        goto done;
    }

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

    if (psurf->priv->wsurf != NULL)
    {
        wl_proxy_wrapper_destroy(psurf->priv->wsurf);
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

    DestroySurfaceFeedback(psurf);

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

static void on_frame_done(void *userdata, struct wl_callback *callback, uint32_t callback_data)
{
    EplSurface *psurf = userdata;

    if (psurf->priv->current.frame_callback == callback)
    {
        wl_callback_destroy(psurf->priv->current.frame_callback);
        psurf->priv->current.frame_callback = NULL;
    }
}
static const struct wl_callback_listener FRAME_CALLBACK_LISTENER = { on_frame_done };

/**
 * Waits for any previous frames.
 *
 * This function ensures that the client doesn't run too far ahead of the
 * compositor.
 *
 * Currently, this just uses a wl_surface::frame request.
 */
static EGLBoolean WaitForPreviousFrames(EplSurface *psurf)
{
    while (psurf->priv->current.frame_callback != NULL)
    {
        if (wl_display_dispatch_queue(psurf->priv->inst->wdpy, psurf->priv->current.queue) < 0)
        {
            eplSetError(psurf->priv->inst->platform, EGL_BAD_ALLOC,
                    "Failed to dispatch Wayland events");
            return EGL_FALSE;
        }
    }

    return EGL_TRUE;
}

/**
 * Sets up a fence for client -> server synchronization.
 *
 * If we've got explicit sync, then this function will attach a fence to the
 * timeline object, but it will NOT send the set_acquire_point or
 * set_release_point request. The current timeline point will be set to the
 * acquire point.
 */
static EGLBoolean SyncRendering(EplSurface *psurf, WlPresentBuffer *present_buf)
{
    EGLSync sync = EGL_NO_SYNC;
    int syncFd = -1;
    EGLBoolean success = EGL_FALSE;

    if (!psurf->priv->inst->supports_EGL_ANDROID_native_fence_sync)
    {
        // If we don't have EGL_ANDROID_native_fence_sync, then we can't do
        // anything other than a glFinish here.
        assert(psurf->priv->current.syncobj == NULL);
        psurf->priv->inst->platform->priv->egl.Finish();
        return EGL_TRUE;
    }

    sync = psurf->priv->inst->platform->priv->egl.CreateSync(psurf->priv->inst->internal_display->edpy,
            EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (sync == EGL_NO_SYNC)
    {
        goto done;
    }
    psurf->priv->inst->platform->priv->egl.Flush();

    syncFd = psurf->priv->inst->platform->priv->egl.DupNativeFenceFDANDROID(psurf->priv->inst->internal_display->edpy, sync);
    if (syncFd < 0)
    {
        goto done;
    }

    if (psurf->priv->current.syncobj != NULL)
    {
        assert(present_buf->timeline.wtimeline != NULL);

        /*
         * We've got explicit sync available, so plug the syncfd into the next
         * timeline point.
         *
         * We let the caller send the set_acquire/release_point requests,
         * though. That makes it easier to ensure that the sync requests are
         * always sent alongside attach and commit requests.
         */
        success = eplWlTimelineAttachSyncFD(psurf->priv->inst, &present_buf->timeline, syncFd);
    }
    else
    {
        // Attach an implicit sync fence if we can. If we can't, then fall back
        // to a CPU wait.
        if (present_buf->dmabuf < 0 || !psurf->priv->inst->supports_implicit_sync
                || !eplWlImportDmaBufSyncFile(present_buf->dmabuf, syncFd))
        {
            psurf->priv->inst->platform->priv->egl.Finish();
        }
        success = EGL_TRUE;
    }

done:
    if (sync != EGL_NO_SYNC)
    {
        psurf->priv->inst->platform->priv->egl.DestroySync(psurf->priv->inst->internal_display->edpy, sync);
    }
    if (syncFd >= 0)
    {
        close(syncFd);
    }
    return success;
}

EGLBoolean eplWlSwapBuffers(EplPlatformData *plat, EplDisplay *pdpy,
        EplSurface *psurf, const EGLint *rects, EGLint n_rects)
{
    WlDisplayInstance *inst = pdpy->priv->inst;
    WlPresentBuffer *present_buf = NULL;
    WlSwapChain *new_swapchain = NULL;
    EGLBoolean success = EGL_FALSE;

    if (!WaitForPreviousFrames(psurf))
    {
        return EGL_FALSE;
    }

    pthread_mutex_lock(&psurf->priv->params.mutex);
    if (psurf->priv->params.native_window == NULL)
    {
        pthread_mutex_unlock(&psurf->priv->params.mutex);
        eplSetError(plat, EGL_BAD_NATIVE_WINDOW, "wl_egl_window has been destroyed");
        return EGL_FALSE;
    }

    psurf->priv->params.skip_update_callback++;
    pthread_mutex_unlock(&psurf->priv->params.mutex);

    // If the window has been resized, then allocate a new swapchain. We'll
    // switch to it after presenting.
    if (!SwapChainRealloc(psurf, EGL_TRUE, &new_swapchain))
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Failed to allocate resized buffers");
        goto done;
    }

    if (psurf->priv->current.swapchain->prime)
    {
        // For PRIME, we need to find a free present buffer up front so that we
        // can blit to it.
        present_buf = eplWlSwapChainFindFreePresentBuffer(inst,
                psurf->priv->current.swapchain, psurf->priv->current.queue);
        if (present_buf == NULL)
        {
            goto done;
        }
        if (!plat->priv->egl.PlatformCopyColorBufferNVX(inst->internal_display->edpy,
                psurf->priv->current.swapchain->render_buffer,
                present_buf->buffer))
        {
            eplSetError(plat, EGL_BAD_ALLOC, "Driver error: Failed to blit to shared wl_buffer");
            goto done;
        }
    }
    else
    {
        // For non-PRIME, we can present the current back buffer directly. We
        // don't need a new back buffer until after presenting (which might
        // free up an existing buffer).
        present_buf = psurf->priv->current.swapchain->current_back;
    }

    if (!SyncRendering(psurf, present_buf))
    {
        goto done;
    }

    if (rects != NULL && n_rects > 0
            && wl_proxy_get_version((struct wl_proxy *) psurf->priv->wsurf) >= 3)
    {
        EGLint i;
        for (i=0; i<n_rects; i++)
        {
            const EGLint *rect = rects + (i * 4);
            wl_surface_damage_buffer(psurf->priv->wsurf,
                    rect[0], rect[1], rect[2], rect[3]);
        }
    }
    else
    {
        wl_surface_damage(psurf->priv->wsurf, 0, 0, INT_MAX, INT_MAX);
    }

    psurf->priv->current.frame_callback = wl_surface_frame(psurf->priv->wsurf);
    if (psurf->priv->current.frame_callback != NULL)
    {
        wl_callback_add_listener(psurf->priv->current.frame_callback,
                &FRAME_CALLBACK_LISTENER, psurf);
    }

    if (psurf->priv->current.syncobj != NULL)
    {
        assert(present_buf->timeline.wtimeline != NULL);

        wp_linux_drm_syncobj_surface_v1_set_acquire_point(psurf->priv->current.syncobj,
                present_buf->timeline.wtimeline,
                (uint32_t) (present_buf->timeline.point >> 32),
                (uint32_t) present_buf->timeline.point);

        present_buf->timeline.point++;
        wp_linux_drm_syncobj_surface_v1_set_release_point(psurf->priv->current.syncobj,
                present_buf->timeline.wtimeline,
                (uint32_t) (present_buf->timeline.point >> 32),
                (uint32_t) present_buf->timeline.point);
    }

    wl_surface_attach(psurf->priv->wsurf, present_buf->wbuf, 0, 0);
    wl_surface_commit(psurf->priv->wsurf);
    wl_display_flush(psurf->priv->inst->wdpy);
    present_buf->status = BUFFER_STATUS_IN_USE;

    if (new_swapchain != NULL)
    {
        SetWindowSwapchain(psurf, new_swapchain);
        new_swapchain = NULL;
    }
    else if (!psurf->priv->current.swapchain->prime)
    {
        // For non-PRIME, find a free buffer to use as the new back buffer.
        WlPresentBuffer *next_back = eplWlSwapChainFindFreePresentBuffer(inst,
                psurf->priv->current.swapchain, psurf->priv->current.queue);
        EGLAttrib buffers[] = { GL_BACK, 0, EGL_NONE };

        if (next_back == NULL)
        {
            psurf->priv->current.force_realloc = EGL_TRUE;
            goto done;
        }

        buffers[1] = (EGLAttrib) next_back->buffer;

        if (!psurf->priv->inst->platform->priv->egl.PlatformSetColorBuffersNVX(
                    psurf->priv->inst->internal_display->edpy,
                    psurf->internal_surface, buffers))
        {
            // Note that this should nver fail. The surface is the same size,
            // so the driver doesn't have to reallocate anything.
            psurf->priv->current.force_realloc = EGL_TRUE;
            goto done;
        }

        psurf->priv->current.swapchain->current_back = next_back;
        psurf->priv->current.swapchain->render_buffer = next_back->buffer;
    }

    // Note that for PRIME, since we don't have a front buffer at all, so we
    // can just keep using the same back buffer.

    success = EGL_TRUE;

done:
    if (new_swapchain != NULL)
    {
        eplWlSwapChainDestroy(psurf->priv->inst, new_swapchain);
    }
    pthread_mutex_lock(&psurf->priv->params.mutex);
    psurf->priv->params.skip_update_callback--;
    pthread_mutex_unlock(&psurf->priv->params.mutex);

    return success;
}
