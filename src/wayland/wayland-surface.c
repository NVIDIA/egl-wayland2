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

#include "platform-utils.h"
#include "wayland-platform.h"
#include "wayland-display.h"
#include "wayland-swapchain.h"
#include "wayland-dmabuf.h"
#include "wl-object-utils.h"

static const int WL_EGL_WINDOW_DESTROY_CALLBACK_SINCE = 3;

/**
 * How much time to use for padding in wp_commit_timer_v1::set_timestamp.
 */
static const uint32_t FRAME_TIMESTAMP_PADDING = 500000; // 5 ms

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

    /**
     * The color format that we're using for this window.
     *
     * This is an entry in the driver's format list.
     */
    const WlDmaBufFormat *driver_format;

    /**
     * The fourcc format code that we'll send to the server for presentation.
     */
    uint32_t present_fourcc;

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

        /// A wrapper for the app's wl_surface.
        struct wl_surface *wsurf;

        /// A wrapper for the display's wp_presentation object.
        struct wp_presentation *presentation_time;

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
         * A callback for the last presentation.
         */
        struct wl_callback *frame_callback;

        /**
         * A callback for a wl_display::sync request after the previous
         * eglSwapBuffers.
         */
        struct wl_callback *last_swap_sync;

        /**
         * A presentation feedback object for the last presented frame.
         *
         * If this is non-NULL, then we can expect to receive a presented or
         * discarded event in finite time.
         *
         * Currently, that means we've got a wp_fifo_v1 object, and the last
         * eglSwapBuffers had a nonzero swap interval.
         */
        struct wp_presentation_feedback *presentation_feedback;

        struct wp_fifo_v1 *fifo;
        struct wp_commit_timer_v1 *commit_timer;

        /**
         * The timestamp of the last wp_presentation_feedback::presented or
         * discarded event.
         *
         * This is used to set a commit time with wp_commit_timer_v1.
         */
        uint64_t last_present_timestamp;

        /**
         * The refresh rate reported in the last wp_presentation_feedback::presented
         * event.
         *
         * If we haven't received any presented events, then we'll use a
         * default value of 60 Hz.
         */
        uint32_t last_present_refresh;

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
            psurf->priv->inst->default_feedback->num_formats, psurf->priv->present_fourcc);

    if (server_format == NULL)
    {
        // This should never happen unless we're using a different format than
        // the EGLConfig: If we didn't find server support for this format,
        // then we should never have set EGL_WINDOW_BIT for the EGLConfig.
        assert(psurf->priv->present_fourcc != driver_format->fourcc);
        return;
    }

    for (i=0; i<driver_format->num_modifiers; i++)
    {
        if (eplWlDmaBufFormatSupportsModifier(server_format, driver_format->modifiers[i]))
        {
            psurf->priv->current.surface_modifiers[psurf->priv->current.num_surface_modifiers++] = driver_format->modifiers[i];
        }
    }
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
        if (state->base.format_table[*index].fourcc != psurf->priv->present_fourcc)
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

    if (inst->force_prime || wl_proxy_get_version((struct wl_proxy *) inst->globals.dmabuf)
            < ZWP_LINUX_DMABUF_V1_GET_SURFACE_FEEDBACK_SINCE_VERSION)
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
    state->feedback = zwp_linux_dmabuf_v1_get_surface_feedback(wrapper, psurf->priv->current.wsurf);
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
            swapchain = eplWlSwapChainCreate(psurf->priv->inst, psurf->priv->current.wsurf,
                    width, height, driver_format->fourcc, psurf->priv->present_fourcc, EGL_FALSE,
                    psurf->priv->current.surface_modifiers,
                    psurf->priv->current.num_surface_modifiers);
        }
        else
        {
            swapchain = eplWlSwapChainCreate(psurf->priv->inst, psurf->priv->current.wsurf,
                    width, height, driver_format->fourcc, psurf->priv->present_fourcc, EGL_TRUE,
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

static uint32_t FindOpaqueFormat(const EplFormatInfo *fmt)
{
    int i;

    if (fmt->colors[3] == 0)
    {
        // This is already an opaque format, so just use it as-is.
        return fmt->fourcc;
    }

    // Look for a format which has the same bits per pixel, the same RGB sizes
    // and offsets, and zero for alpha.
    for (i=0; i<FORMAT_INFO_COUNT; i++)
    {
        const EplFormatInfo *other = &FORMAT_INFO_LIST[i];
        if (other->bpp == fmt->bpp
                && other->colors[0] == fmt->colors[0]
                && other->colors[1] == fmt->colors[1]
                && other->colors[2] == fmt->colors[2]
                && other->colors[3] == 0
                && other->offset[0] == fmt->offset[0]
                && other->offset[1] == fmt->offset[1]
                && other->offset[2] == fmt->offset[2])
        {
            return other->fourcc;
        }
    }

    return DRM_FORMAT_INVALID;
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
    uint32_t wsurf_id;
    const EplConfig *configInfo = NULL;
    const WlDmaBufFormat *driver_format = NULL;
    EGLSurface internalSurface = EGL_NO_SURFACE;
    EGLAttrib *driverAttribs = NULL;
    EGLint numAttribs = eplCountAttribs(attribs);
    EGLBoolean presentOpaque = EGL_FALSE;
    EGLAttrib platformAttribs[] =
    {
        GL_BACK, 0,
        EGL_PLATFORM_SURFACE_UPDATE_CALLBACK_NVX, (EGLAttrib) WindowUpdateCallback,
        EGL_PLATFORM_SURFACE_UPDATE_CALLBACK_PARAM_NVX, (EGLAttrib) psurf,
        EGL_NONE
    };

    if (!wlEglGetWindowVersionAndSurface(window, &windowVersion, &wsurf))
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
    wsurf_id = wl_proxy_get_id((struct wl_proxy *) wsurf);
    glvnd_list_for_each_entry(otherSurf, existing_surfaces, entry)
    {
        if (otherSurf->type == EPL_SURFACE_TYPE_WINDOW && otherSurf->priv != NULL
                && wl_proxy_get_id((struct wl_proxy *) otherSurf->priv->current.wsurf) == wsurf_id)
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

    driverAttribs = malloc((numAttribs + 3) * sizeof(EGLAttrib));
    if (driverAttribs == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Out of memory");
        goto done;
    }

    numAttribs = 0;
    if (attribs != NULL && attribs[0] != EGL_NONE)
    {
        int i;

        for (i = 0; attribs[i] != EGL_NONE; i += 2)
        {
            if (attribs[i] == EGL_PRESENT_OPAQUE_EXT)
            {
                presentOpaque = (attribs[i + 1] != 0);
            }
            else if (attribs[i] == EGL_SURFACE_Y_INVERTED_NVX)
            {
                eplSetError(plat, EGL_BAD_ATTRIBUTE, "Invalid attribute 0x%04x\n", attribs[i]);
                goto done;
            }
            else
            {
                driverAttribs[numAttribs++] = attribs[i];
                driverAttribs[numAttribs++] = attribs[i + 1];
            }
        }
    }
    driverAttribs[numAttribs++] = EGL_SURFACE_Y_INVERTED_NVX;
    driverAttribs[numAttribs++] = EGL_TRUE;
    driverAttribs[numAttribs] = EGL_NONE;

    // Allocate enough space for the EplImplSurface, plus extra to hold a
    // format modifier list.
    priv = calloc(1, sizeof(EplImplSurface)
            + driver_format->num_modifiers * sizeof(uint64_t));
    if (priv == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Out of memory");
        goto done;
    }

    if (pthread_mutex_init(&priv->params.mutex, NULL) != 0)
    {
        free(priv);
        priv = NULL;
        eplSetError(plat, EGL_BAD_ALLOC, "Failed to create internal mutex");
        goto done;
    }

    psurf->priv = priv;
    priv->current.surface_modifiers = (uint64_t *) (priv + 1);

    // Until we get a wp_presentation_feedback::presented event, start by
    // assuming a refresh rate of 60 Hz.
    priv->current.last_present_refresh = (1000000000 / 60);
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
    }
    if (priv->current.queue == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Failed to create internal event queue");
        goto done;
    }

    priv->current.wsurf = wl_proxy_create_wrapper(wsurf);
    if (priv->current.wsurf == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Failed to create internal wl_surface wrapper");
        goto done;
    }
    wl_proxy_set_queue((struct wl_proxy *) priv->current.wsurf, priv->current.queue);

    priv->native_window_version = windowVersion;
    priv->driver_format = driver_format;
    priv->present_fourcc = driver_format->fourcc;
    if (presentOpaque)
    {
        priv->present_fourcc = FindOpaqueFormat(driver_format->fmt);
        if (priv->present_fourcc == DRM_FORMAT_INVALID)
        {
            // This should never happen: Every entry in FORMAT_INFO_LIST should
            // either be opaque or have a corresponding opaque format.
            eplSetError(plat, EGL_BAD_ALLOC, "Internal error: Can't find opaque format for EGLConfig");
            goto done;
        }
    }

    priv->params.native_window = window;
    priv->params.swap_interval = 1;
    priv->params.pending_width = (window->width > 0 ? window->width : 1);
    priv->params.pending_height = (window->height > 0 ? window->height : 1);

    if (inst->globals.syncobj != NULL)
    {
        priv->current.syncobj = wp_linux_drm_syncobj_manager_v1_get_surface(inst->globals.syncobj, priv->current.wsurf);
        if (priv->current.syncobj == NULL)
        {
            goto done;
        }
    }

    if (inst->globals.fifo != NULL && inst->globals.presentation_time != NULL)
    {
        priv->current.presentation_time = wl_proxy_create_wrapper(inst->globals.presentation_time);
        if (priv->current.presentation_time == NULL)
        {
            eplSetError(plat, EGL_BAD_ALLOC, "Failed to create wp_presentation wrapper");
            goto done;
        }
        wl_proxy_set_queue((struct wl_proxy *) priv->current.presentation_time, priv->current.queue);

        priv->current.fifo = wp_fifo_manager_v1_get_fifo(inst->globals.fifo, priv->current.wsurf);
        if (priv->current.fifo == NULL)
        {
            goto done;
        }

        if (inst->globals.commit_timing != NULL)
        {
            priv->current.commit_timer = wp_commit_timing_manager_v1_get_timer(inst->globals.commit_timing, priv->current.wsurf);
            if (priv->current.commit_timer == NULL)
            {
                goto done;
            }
        }
    }

    // Initialize the modifier list based on the default modifiers.
    PickDefaultModifiers(psurf);
    if (psurf->priv->current.num_surface_modifiers == 0)
    {
        /*
         * If we didn't find any shared modifiers, then check if the server
         * supports linear. If it does, then we can use the prime path instead.
         */
        const WlDmaBufFormat *server_format = eplWlDmaBufFormatFind(psurf->priv->inst->default_feedback->formats,
                psurf->priv->inst->default_feedback->num_formats, psurf->priv->present_fourcc);
        if (server_format == NULL
                || !eplWlDmaBufFormatSupportsModifier(server_format, DRM_FORMAT_MOD_LINEAR))
        {
            /*
             * If the app set the EGL_PRESENT_OPAQUE_EXT, then the format we're
             * sending to the server might be different than the format for the
             * EGLConfig.
             *
             * In that case, it's possible (if unlikely) that the server could
             * have different modifier support.
             *
             * If we're using the same modifier as the EGLConfig, then we
             * shouldn't get here, because the EGL_WINDOW_BIT flag should not
             * have been set.
             */
            assert(psurf->priv->present_fourcc != driver_format->fourcc);
            eplSetError(plat, EGL_BAD_ALLOC, "No supported format modifiers for opaque format 0x%08x\n",
                    psurf->priv->present_fourcc);
            goto done;
        }
    }

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
            config, platformAttribs, driverAttribs);
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
    free(driverAttribs);
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

    if (psurf->priv->current.wsurf != NULL)
    {
        wl_proxy_wrapper_destroy(psurf->priv->current.wsurf);
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
    if (psurf->priv->current.last_swap_sync != NULL)
    {
        wl_callback_destroy(psurf->priv->current.last_swap_sync);
    }
    if (psurf->priv->current.presentation_feedback != NULL)
    {
        wp_presentation_feedback_destroy(psurf->priv->current.presentation_feedback);
    }
    if (psurf->priv->current.fifo != NULL)
    {
        wp_fifo_v1_destroy(psurf->priv->current.fifo);
    }
    if (psurf->priv->current.commit_timer != NULL)
    {
        wp_commit_timer_v1_destroy(psurf->priv->current.commit_timer);
    }
    if (psurf->priv->current.presentation_time != NULL)
    {
        wl_proxy_wrapper_destroy(psurf->priv->current.presentation_time);
    }
    if (psurf->priv->current.queue != NULL)
    {
        wl_event_queue_destroy(psurf->priv->current.queue);
    }

    pthread_mutex_destroy(&psurf->priv->params.mutex);

    eplWlDisplayInstanceUnref(psurf->priv->inst);
    free(psurf->priv);
    psurf->priv = NULL;
}

static void on_frame_done(void *userdata, struct wl_callback *callback, uint32_t callback_data)
{
    EplSurface *psurf = userdata;

    if (psurf->priv->current.frame_callback == callback)
    {
        psurf->priv->current.frame_callback = NULL;
    }
    if (psurf->priv->current.last_swap_sync == callback)
    {
        psurf->priv->current.last_swap_sync = NULL;
    }
    wl_callback_destroy(callback);
}
static const struct wl_callback_listener FRAME_CALLBACK_LISTENER = { on_frame_done };

static void on_wp_presentation_feedback_sync_output(void *userdata,
        struct wp_presentation_feedback *wfeedback, struct wl_output *output)
{
}
static void DiscardPresentationFeedback(EplSurface *psurf)
{
    struct timespec ts;
    if (clock_gettime(psurf->priv->inst->presentation_time_clock_id, &ts) == 0)
    {
        psurf->priv->current.last_present_timestamp = ((uint64_t) ts.tv_sec) * 1000000000 + ts.tv_nsec;
    }

    wp_presentation_feedback_destroy(psurf->priv->current.presentation_feedback);
    psurf->priv->current.presentation_feedback = NULL;
}
static void on_wp_presentation_feedback_discarded(void *userdata,
        struct wp_presentation_feedback *wfeedback)
{
    EplSurface *psurf = userdata;

    assert(wfeedback == psurf->priv->current.presentation_feedback);
    DiscardPresentationFeedback(psurf);
}
static void on_wp_presentation_feedback_presented(void *userdata,
        struct wp_presentation_feedback *wfeedback,
        uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec,
        uint32_t refresh, uint32_t seq_hi, uint32_t seq_lo, uint32_t flags)
{
    EplSurface *psurf = userdata;

    assert(wfeedback == psurf->priv->current.presentation_feedback);

    psurf->priv->current.last_present_timestamp = (((uint64_t) tv_sec_hi) << 32 | tv_sec_lo) + tv_nsec;
    psurf->priv->current.last_present_refresh = refresh;

    wp_presentation_feedback_destroy(psurf->priv->current.presentation_feedback);
    psurf->priv->current.presentation_feedback = NULL;
}
static const struct wp_presentation_feedback_listener PRESENTATION_FEEDBACK_LISTENER =
{
    on_wp_presentation_feedback_sync_output,
    on_wp_presentation_feedback_presented,
    on_wp_presentation_feedback_discarded,
};

/**
 * Waits for any previous frames.
 *
 * This function ensures that the client doesn't run too far ahead of the
 * compositor.
 */
static EGLBoolean WaitForPreviousFrames(EplSurface *psurf)
{
    while (psurf->priv->current.frame_callback != NULL
            || psurf->priv->current.last_swap_sync != NULL
            || psurf->priv->current.presentation_feedback != NULL)
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
    struct wl_display *wdpy_wrapper = NULL;
    EGLint swap_interval;

    pthread_mutex_lock(&psurf->priv->params.mutex);
    if (psurf->priv->params.native_window == NULL)
    {
        pthread_mutex_unlock(&psurf->priv->params.mutex);
        eplSetError(plat, EGL_BAD_NATIVE_WINDOW, "wl_egl_window has been destroyed");
        return EGL_FALSE;
    }

    swap_interval = psurf->priv->params.swap_interval;
    psurf->priv->params.skip_update_callback++;
    pthread_mutex_unlock(&psurf->priv->params.mutex);

    // Dispatch any pending events, but don't block for them. This will ensure
    // that we pick up any modifier changes that the server might have sent.
    wl_display_dispatch_queue_pending(psurf->priv->inst->wdpy, psurf->priv->current.queue);

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
                psurf->priv->current.swapchain);
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

    if (swap_interval > 0)
    {
        if (!WaitForPreviousFrames(psurf))
        {
            return EGL_FALSE;
        }
    }
    else
    {
        // If the swap interval is zero, then don't wait for a previous frame.
        // Try to present immediately.
        if (psurf->priv->current.presentation_feedback != NULL)
        {
            // If we still have an outstanding presentation, then treat this as
            // a discarded frame, and use the current time as the last
            // presentation time.
            DiscardPresentationFeedback(psurf);
        }
        if (psurf->priv->current.frame_callback != NULL)
        {
            wl_callback_destroy(psurf->priv->current.frame_callback);
            psurf->priv->current.frame_callback = NULL;
        }
        if (psurf->priv->current.last_swap_sync != NULL)
        {
            wl_callback_destroy(psurf->priv->current.last_swap_sync);
            psurf->priv->current.last_swap_sync = NULL;
        }
    }

    assert(psurf->priv->current.presentation_feedback == NULL);
    assert(psurf->priv->current.frame_callback == NULL);
    assert(psurf->priv->current.last_swap_sync == NULL);

    if (rects != NULL && n_rects > 0
            && wl_proxy_get_version((struct wl_proxy *) psurf->priv->current.wsurf)
                >= WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
    {
        EGLint i;
        for (i=0; i<n_rects; i++)
        {
            const EGLint *rect = rects + (i * 4);
            wl_surface_damage_buffer(psurf->priv->current.wsurf,
                    rect[0], rect[1], rect[2], rect[3]);
        }
    }
    else
    {
        wl_surface_damage(psurf->priv->current.wsurf, 0, 0, INT_MAX, INT_MAX);
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

    wl_surface_attach(psurf->priv->current.wsurf, present_buf->wbuf, 0, 0);

    if (psurf->priv->current.presentation_time != NULL && psurf->priv->current.fifo != NULL)
    {
        wp_fifo_v1_set_barrier(psurf->priv->current.fifo);

        if (swap_interval > 0)
        {
            if (psurf->priv->current.commit_timer != NULL
                    && psurf->priv->current.last_present_timestamp != 0)
            {
                uint64_t timestamp = ((uint64_t) swap_interval) * psurf->priv->current.last_present_refresh;
                if (timestamp >= FRAME_TIMESTAMP_PADDING)
                {
                    uint64_t sec;
                    uint32_t nsec;

                    timestamp += psurf->priv->current.last_present_timestamp - FRAME_TIMESTAMP_PADDING;
                    sec = timestamp / 1000000000;
                    nsec = timestamp % 1000000000;
                    wp_commit_timer_v1_set_timestamp(psurf->priv->current.commit_timer,
                            (uint32_t) (sec >> 32), (uint32_t) sec, nsec);
                }
            }

            psurf->priv->current.presentation_feedback = wp_presentation_feedback(
                    psurf->priv->current.presentation_time, psurf->priv->current.wsurf);
            if (psurf->priv->current.presentation_feedback != NULL)
            {
                wp_presentation_feedback_add_listener(psurf->priv->current.presentation_feedback,
                        &PRESENTATION_FEEDBACK_LISTENER, psurf);
            }


            wp_fifo_v1_wait_barrier(psurf->priv->current.fifo);

            /*
             * If the window is not visible (occluded, monitor on standby,
             * etc), then we could be waiting for an indefinite amount of time
             * for the compositor to send a wp_presentation_feedback::presented
             * or discarded event.
             *
             * But, wp_fifo_v1 is required to unblock in finite time, so we can
             * send an extra dummy commit with a wp_fifo_v1::wait_barrier.
             *
             * If the window is visible, then the compositor will send a
             * presented event as normal, and if the window is not visible,
             * then the second commit will trigger a discarded event.
             *
             * Note that the compositor may trigger a discarded event
             * immediately, so we use wp_commit_timer_v1 above to try to
             * throttle things to a sane rate.
             *
             * Ugly as this is, Mesa relies on the same behavior, so it's
             * probably safe to treat this as the "intended" behavior.
             */
            wl_surface_commit(psurf->priv->current.wsurf);
            wp_fifo_v1_wait_barrier(psurf->priv->current.fifo);
        }
    }
    else
    {
        // If we don't have FIFO or presentation time support, then just
        // request a frame callback.
        psurf->priv->current.frame_callback = wl_surface_frame(psurf->priv->current.wsurf);
        if (psurf->priv->current.frame_callback != NULL)
        {
            wl_callback_add_listener(psurf->priv->current.frame_callback,
                    &FRAME_CALLBACK_LISTENER, psurf);
        }
    }

    wl_surface_commit(psurf->priv->current.wsurf);

    pthread_mutex_lock(&psurf->priv->params.mutex);
    if (psurf->priv->params.native_window != NULL)
    {
        psurf->priv->params.native_window->attached_width = psurf->priv->current.swapchain->width;
        psurf->priv->params.native_window->attached_height = psurf->priv->current.swapchain->height;
    }
    pthread_mutex_unlock(&psurf->priv->params.mutex);

    /*
     * Send a wl_display::sync request after the commit.
     *
     * If we don't have FIFO support, or if the swap interval is zero, then we
     * can't safely use the presentation timing event in eglWaitGL, but we can
     * at least wait to make sure that the server has received the present
     * requests.
     */
    wdpy_wrapper = wl_proxy_create_wrapper(psurf->priv->inst->wdpy);
    if (wdpy_wrapper != NULL)
    {
        wl_proxy_set_queue((struct wl_proxy *) wdpy_wrapper, psurf->priv->current.queue);
        psurf->priv->current.last_swap_sync = wl_display_sync(wdpy_wrapper);
        wl_proxy_wrapper_destroy(wdpy_wrapper);
        if (psurf->priv->current.last_swap_sync != NULL)
        {
            wl_callback_add_listener(psurf->priv->current.last_swap_sync,
                    &FRAME_CALLBACK_LISTENER, psurf);
        }
    }

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
                psurf->priv->current.swapchain);
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

        eplWlSwapChainUpdateBufferAge(inst, psurf->priv->current.swapchain, present_buf);

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

EGLBoolean eplWlSwapInterval(EplDisplay *pdpy, EplSurface *psurf, EGLint interval)
{
    if (psurf->type == EPL_SURFACE_TYPE_WINDOW)
    {
        if (interval < 0)
        {
            interval = 0;
        }

        pthread_mutex_lock(&psurf->priv->params.mutex);
        psurf->priv->params.swap_interval = interval;
        pthread_mutex_unlock(&psurf->priv->params.mutex);
    }
    return EGL_TRUE;
}

EGLBoolean eplWlWaitGL(EplDisplay *pdpy, EplSurface *psurf)
{
    EGLBoolean ret = EGL_TRUE;

    pdpy->platform->priv->egl.Finish();
    if (psurf != NULL && psurf->type == EPL_SURFACE_TYPE_WINDOW)
    {
        /*
         * Wait until the server has received the commit from the last
         * eglSwapBuffers.
         *
         * If possible, we'll also wait for the presentation feedback so that
         * the last frame is actually on screen.
         *
         * Note that if we don't have presentation timing support, then we do
         * NOT wait for a wl_surface::frame callback, because that could block
         * forever.
         */

        while (psurf->priv->current.presentation_feedback != NULL
                || psurf->priv->current.last_swap_sync != NULL)
        {
            if (wl_display_dispatch_queue(psurf->priv->inst->wdpy, psurf->priv->current.queue) < 0)
            {
                eplSetError(psurf->priv->inst->platform, EGL_BAD_ALLOC,
                        "Failed to dispatch Wayland events");
                return EGL_FALSE;
            }
        }
    }

    return ret;
}

EGLint eplWlQueryBufferAge(EplDisplay *pdpy, EplSurface *psurf)
{
    if (psurf->priv->current.swapchain->prime)
    {
        return 0;
    }
    else
    {
        return psurf->priv->current.swapchain->current_back->buffer_age;
    }
}
