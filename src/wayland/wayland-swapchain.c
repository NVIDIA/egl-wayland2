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

#include "wayland-swapchain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <limits.h>
#include <assert.h>

#include <GL/gl.h>

/**
 * The maximum number of color buffers to allocate for a window.
 */
static const size_t MAX_PRESENT_BUFFERS = 4;

/**
 * How long to wait for a buffer release before we stop to check for window
 * events.
 */
static const int RELEASE_WAIT_TIMEOUT = 100;

static void DestroyPresentBuffer(WlDisplayInstance *inst, WlPresentBuffer *buffer)
{
    if (buffer != NULL)
    {
        if (buffer->wbuf != NULL)
        {
            wl_buffer_destroy(buffer->wbuf);
        }
        if (buffer->dmabuf >= 0)
        {
            close(buffer->dmabuf);
        }
        if (buffer->buffer != NULL)
        {
            inst->platform->priv->egl.PlatformFreeColorBufferNVX(inst->internal_display->edpy, buffer->buffer);
        }

        eplWlTimelineDestroy(inst, &buffer->timeline);

        free(buffer);
    }
}

static void on_buffer_release(void *userdata, struct wl_buffer *wbuf)
{
    WlSwapChain *swapchain = userdata;
    WlPresentBuffer *buffer = NULL;

    glvnd_list_for_each_entry(buffer, &swapchain->present_buffers, entry)
    {
        if (buffer->wbuf == wbuf)
        {
            if (buffer->status == BUFFER_STATUS_IN_USE)
            {
                buffer->status = BUFFER_STATUS_IDLE_NOTIFIED;
            }

            /*
             * Move the buffer to the end of the list. If we don't have any
             * server -> client synchronization, then this ensures that
             * we'll reuse the oldest buffers first, so we'll have the best
             * chance that the buffer really is idle.
             */
            glvnd_list_del(&buffer->entry);
            glvnd_list_append(&buffer->entry, &swapchain->present_buffers);
            break;
        }
    }
}
static const struct wl_buffer_listener BUFFER_LISTENER = { on_buffer_release };

typedef struct
{
    struct wl_buffer *buffer;
    EGLBoolean done;
} DmaBufParamsCreateState;

void on_dmabuf_params_created(void *userdata,
        struct zwp_linux_buffer_params_v1 *params,
        struct wl_buffer *buffer)
{
    DmaBufParamsCreateState *state = userdata;
    state->buffer = buffer;
    state->done = EGL_TRUE;
}
void on_dmabuf_params_failed(void *userdata,
           struct zwp_linux_buffer_params_v1 *params)
{
    DmaBufParamsCreateState *state = userdata;
    state->buffer = NULL;
    state->done = EGL_TRUE;
}
static const struct zwp_linux_buffer_params_v1_listener DMABUF_PARAMS_LISTENER =
{
    on_dmabuf_params_created,
    on_dmabuf_params_failed,
};

/**
 * Creates a wl_buffer from a dma-buf.
 */
static struct wl_buffer *ShareDmaBuf(WlDisplayInstance *inst,
        struct wl_event_queue *queue, int dmabuf, uint32_t width, uint32_t height,
        uint32_t stride, uint32_t offset, uint32_t fourcc, uint64_t modifier)
{
    DmaBufParamsCreateState state = {};
    struct zwp_linux_dmabuf_v1 *wrapper = NULL;
    struct zwp_linux_buffer_params_v1 *params = NULL;

    wrapper = wl_proxy_create_wrapper(inst->globals.dmabuf);
    if (wrapper == NULL)
    {
        goto done;
    }

    wl_proxy_set_queue((struct wl_proxy *) wrapper, queue);
    params = zwp_linux_dmabuf_v1_create_params(wrapper);
    if (params == NULL)
    {
        goto done;
    }
    zwp_linux_buffer_params_v1_add_listener(params, &DMABUF_PARAMS_LISTENER, &state);

    // Note that libwayland-client will duplicate the file descriptor, so we
    // don't need to duplicate it here.
    zwp_linux_buffer_params_v1_add(params, dmabuf, 0, (uint32_t) offset, (uint32_t) stride,
            (uint32_t) (modifier >> 32), (uint32_t) (modifier & 0xFFFFFFFF));

    zwp_linux_buffer_params_v1_create(params, width, height, fourcc, 0);

    while (!state.done)
    {
        if (wl_display_roundtrip_queue(inst->wdpy, queue) < 0)
        {
            goto done;
        }
    }

done:
    if (params != NULL)
    {
        zwp_linux_buffer_params_v1_destroy(params);
    }
    if (wrapper != NULL)
    {
        wl_proxy_wrapper_destroy(wrapper);
    }

    return state.buffer;
}

/**
 * Adds a WlPresentBuffer to a swapchain from a dma-buf.
 *
 * \note This function will either close or take ownership of \p dmabuf, so
 * the caller must not close or use it afterward.
 *
 * \param inst The display instance
 * \param swapchain The swapchain to add the buffer to
 * \param dmabuf The dma-buf file descriptor
 * \param stride The stride value for the dma-buf
 * \param offset The offset value for the dma-buf
 *
 * \return A new WlPresentBuffer struct, or NULL on error.
 */
static WlPresentBuffer *SwapChainAppendPresentBuffer(WlDisplayInstance *inst,
        WlSwapChain *swapchain, int dmabuf, uint32_t stride, uint32_t offset)
{
    WlPresentBuffer *buf = calloc(1, sizeof(WlPresentBuffer));

    if (buf == NULL)
    {
        close(dmabuf);
        return NULL;
    }

    glvnd_list_init(&buf->entry);
    buf->dmabuf = dmabuf;
    buf->status = BUFFER_STATUS_IDLE;

    if (inst->globals.syncobj != NULL)
    {
        if (!eplWlTimelineInit(inst, &buf->timeline))
        {
            DestroyPresentBuffer(inst, buf);
            return NULL;
        }
    }

    buf->wbuf = ShareDmaBuf(inst, swapchain->queue, dmabuf, swapchain->width, swapchain->height,
            stride, offset, swapchain->present_fourcc, swapchain->modifier);
    if (buf->wbuf == NULL)
    {
        DestroyPresentBuffer(inst, buf);
        return NULL;
    }
    if (inst->globals.syncobj != NULL)
    {
        // If we have explicit sync, then we don't need to keep the dma-buf
        // open.
        close(buf->dmabuf);
        buf->dmabuf = -1;
    }
    else
    {
        // If we don't have explicit sync, then we'll need to watch for
        // wl_buffer::release events.
        wl_buffer_add_listener(buf->wbuf, &BUFFER_LISTENER, swapchain);

        if (!inst->supports_implicit_sync)
        {
            // If we don't have implicit sync either, then we don't have any
            // reason to keep the dma-buf open.
            close(buf->dmabuf);
            buf->dmabuf = -1;
        }
    }

    glvnd_list_add(&buf->entry, &swapchain->present_buffers);

    return buf;
}

WlPresentBuffer *eplWlSwapChainCreatePresentBuffer(WlDisplayInstance *inst,
        WlSwapChain *swapchain)
{
    EGLPlatformColorBufferNVX colorbuf = NULL;
    WlPresentBuffer *presentBuf = NULL;
    int dmabuf = -1;
    int stride, offset;


    colorbuf = inst->platform->priv->egl.PlatformAllocColorBufferNVX(
            inst->internal_display->edpy, swapchain->width, swapchain->height,
            swapchain->render_fourcc, swapchain->modifier, swapchain->prime);
    if (colorbuf == NULL)
    {
        return NULL;
    }

    if (!inst->platform->priv->egl.PlatformExportColorBufferNVX(
                inst->internal_display->edpy, colorbuf, &dmabuf, NULL, NULL, NULL,
                &stride, &offset, NULL))
    {
        inst->platform->priv->egl.PlatformFreeColorBufferNVX(inst->internal_display->edpy, colorbuf);
        return NULL;
    }

    presentBuf = SwapChainAppendPresentBuffer(inst, swapchain, dmabuf, stride, offset);
    if (presentBuf == NULL)
    {
        inst->platform->priv->egl.PlatformFreeColorBufferNVX(inst->internal_display->edpy, colorbuf);
        return NULL;
    }
    presentBuf->buffer = colorbuf;
    return presentBuf;
}

void eplWlSwapChainDestroy(WlDisplayInstance *inst, WlSwapChain *swapchain)
{
    if (swapchain != NULL)
    {
        while (!glvnd_list_is_empty(&swapchain->present_buffers))
        {
            WlPresentBuffer *buffer = glvnd_list_first_entry(&swapchain->present_buffers,
                    WlPresentBuffer, entry);
            glvnd_list_del(&buffer->entry);

            if (buffer->buffer == swapchain->render_buffer)
            {
                swapchain->render_buffer = NULL;
            }

            DestroyPresentBuffer(inst, buffer);
        }

        if (swapchain->queue != NULL)
        {
            wl_event_queue_destroy(swapchain->queue);
        }

        if (swapchain->render_buffer != NULL)
        {
            inst->platform->priv->egl.PlatformFreeColorBufferNVX(inst->internal_display->edpy,
                    swapchain->render_buffer);
        }

        free(swapchain);
    }
}

WlSwapChain *eplWlSwapChainCreate(WlDisplayInstance *inst, struct wl_surface *wsurf,
        uint32_t width, uint32_t height, uint32_t render_fourcc, uint32_t present_fourcc,
        EGLBoolean prime, const uint64_t *modifiers, size_t num_modifiers)
{
    WlSwapChain *swapchain = NULL;
    uint32_t flags = 0;
    struct gbm_bo *gbo = NULL;
    int dmabuf = -1;
    EGLBoolean success = EGL_FALSE;

    swapchain = calloc(1, sizeof(WlSwapChain));
    if (swapchain == NULL)
    {
        goto done;
    }

    glvnd_list_init(&swapchain->present_buffers);
    swapchain->width = width;
    swapchain->height = height;
    swapchain->render_fourcc = render_fourcc;
    swapchain->present_fourcc = present_fourcc;
    swapchain->modifier = DRM_FORMAT_MOD_INVALID;
    swapchain->prime = prime;
    if (inst->platform->priv->wl.display_create_queue_with_name != NULL)
    {
        char name[64];
        snprintf(name, sizeof(name), "EGLSurface(%u/%p)", wl_proxy_get_id((struct wl_proxy *) wsurf), swapchain);
        swapchain->queue = inst->platform->priv->wl.display_create_queue_with_name(inst->wdpy, name);
    }
    else
    {
        swapchain->queue = wl_display_create_queue(inst->wdpy);
    }
    if (swapchain->queue == NULL)
    {
        goto done;
    }

    /*
     * Start by creating the render buffer. We'll do that using libgbm, so that
     * we can let the driver pick an optimal format modifier.
     *
     * After that, we can just eglPlatformAllocColorBufferNVX, and pass it the
     * same modifier as the first buffer we created.
     */

    if (!prime)
    {
        flags |= GBM_BO_USE_SCANOUT;
    }
    if (modifiers != NULL && num_modifiers > 0)
    {
        gbo = inst->platform->priv->gbm.bo_create_with_modifiers2(inst->gbmdev,
                width, height, render_fourcc, modifiers, num_modifiers, flags);
    }
    else
    {
        gbo = gbm_bo_create(inst->gbmdev, width, height, render_fourcc, flags);
    }
    if (gbo == NULL)
    {
        goto done;
    }

    dmabuf = gbm_bo_get_fd(gbo);
    if (dmabuf < 0)
    {
        goto done;
    }

    swapchain->render_buffer = inst->platform->priv->egl.PlatformImportColorBufferNVX(
            inst->internal_display->edpy, dmabuf, width, height, render_fourcc,
            gbm_bo_get_stride(gbo), gbm_bo_get_offset(gbo, 0),
            gbm_bo_get_modifier(gbo));
    if (swapchain->render_buffer == NULL)
    {
        goto done;
    }

    if (prime)
    {
        // For PRIME, we'll have a single renderable buffer, and separate
        // linear present buffers. We don't need to create any present buffers
        // yet -- we can do that in the first call to eglSwapBuffers.
        swapchain->modifier = DRM_FORMAT_MOD_LINEAR;
        close(dmabuf);
    }
    else
    {
        // For non-PRIME, the render buffer is also a present buffer, so set
        // that up now.
        swapchain->modifier = gbm_bo_get_modifier(gbo);
        swapchain->current_back = SwapChainAppendPresentBuffer(inst, swapchain,
                dmabuf, gbm_bo_get_stride(gbo), gbm_bo_get_offset(gbo, 0));
        if (swapchain->current_back == NULL)
        {
            goto done;
        }
        swapchain->current_back->buffer = swapchain->render_buffer;
    }

    success = EGL_TRUE;

done:
    if (!success)
    {
        eplWlSwapChainDestroy(inst, swapchain);
        swapchain = NULL;
    }
    if (gbo != NULL)
    {
        gbm_bo_destroy(gbo);
    }
    return swapchain;
}

/**
 * Waits for a sync FD using eglWaitSync.
 *
 * Using eglWaitSync means that the GPU will wait for the fence, without
 * doing a CPU stall.
 *
 * \param inst The WlDisplayInstance
 * \param syncfd The sync file descriptor, which must be a regular fence. Note
 *      that WaitForSyncFDGPU will take ownership of this fd.
 */
static EGLBoolean WaitForSyncFDGPU(WlDisplayInstance *inst, int syncfd)
{
    EGLBoolean success = EGL_FALSE;

    if (syncfd >= 0)
    {
        const EGLAttrib syncAttribs[] =
        {
            EGL_SYNC_NATIVE_FENCE_FD_ANDROID, syncfd,
            EGL_NONE
        };
        EGLSync sync = inst->platform->priv->egl.CreateSync(inst->internal_display->edpy,
                EGL_SYNC_NATIVE_FENCE_ANDROID, syncAttribs);
        if (sync != EGL_NO_SYNC)
        {
            success = inst->platform->priv->egl.WaitSync(inst->internal_display->edpy, sync, 0);
            inst->platform->priv->egl.DestroySync(inst->internal_display->edpy, sync);
        }
    }
    return success;
}

/**
 * Waits for a timeline point.
 *
 * This will attempt to use eglWaitSync to let the GPU wait on the sync point,
 * but if that fails, then it'll fall back to a CPU wait.
 */
static EGLBoolean WaitTimelinePoint(WlDisplayInstance *inst, WlTimeline *timeline)
{
    int syncfd = eplWlTimelinePointToSyncFD(inst, timeline);
    EGLBoolean success = EGL_FALSE;

    if (syncfd >= 0)
    {
        success = WaitForSyncFDGPU(inst, syncfd);
    }

    if (!success)
    {
        // If using eglWaitSync failed, then just do a CPU wait on the timeline
        // point.
        uint32_t first;
        success = (inst->platform->priv->drm.SyncobjTimelineWait(
                    gbm_device_get_fd(inst->gbmdev),
                    &timeline->handle, &timeline->point, 1, INT64_MAX,
                    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
                    &first) == 0);
        if (!success)
        {
            eplSetError(inst->platform, EGL_BAD_ALLOC,
                    "Internal error: drmSyncobjTimelineWait(WAIT_FOR_SUBMIT) failed: %s\n",
                    strerror(errno));
        }
    }

    return success;
}

static int CheckBufferReleaseExplicit(WlDisplayInstance *inst, WlSwapChain *swapchain, int timeout_ms)
{
    WlPresentBuffer *buffer;
    WlPresentBuffer **buffers;
    uint32_t *handles;
    uint64_t *points;
    int64_t timeout;
    uint32_t count;
    uint32_t first;
    int ret, err;

    count = 0;
    glvnd_list_for_each_entry(buffer, &swapchain->present_buffers, entry)
    {
        if (buffer->status != BUFFER_STATUS_IDLE)
        {
            count++;
        }
    }

    if (count == 0)
    {
        return 0;
    }

    buffers = alloca(count * sizeof(WlPresentBuffer *));
    handles = alloca(count * sizeof(uint32_t));
    points = alloca(count * sizeof(uint64_t));

    count = 0;
    glvnd_list_for_each_entry(buffer, &swapchain->present_buffers, entry)
    {
        if (buffer->status != BUFFER_STATUS_IDLE)
        {
            buffers[count] = buffer;
            handles[count] = buffer->timeline.handle;
            points[count] = buffer->timeline.point;
            count++;
        }
    }

    if (timeout_ms > 0)
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        timeout = ((int64_t) ts.tv_sec) * 1000000000 + ts.tv_nsec;
        timeout += timeout_ms * 1000000;
    }
    else if (timeout_ms < 0)
    {
        timeout = INT64_MAX;
    }
    else
    {
        timeout = 0;
    }

    ret = inst->platform->priv->drm.SyncobjTimelineWait(
                gbm_device_get_fd(inst->gbmdev),
                handles, points, count, timeout,
                DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
                &first);
    err = errno;

    if (ret == 0)
    {
        assert(first < count);
        if (WaitTimelinePoint(inst, &buffers[first]->timeline))
        {
            buffers[first]->status = BUFFER_STATUS_IDLE;
            return count;
        }
        else
        {
            return -1;
        }
    }
    else if (err == ETIME || err == EINTR)
    {
        // Nothing freed up before the timeout, but that's not a fatal error
        // here.
        return count;
    }
    else
    {
        eplSetError(inst->platform, EGL_BAD_ALLOC,
                "Internal error: drmSyncobjTimelineWait(WAIT_AVAILABLE) failed: %s\n",
                strerror(err));
        return -1;
    }
}

static EGLBoolean WaitImplicitFence(WlDisplayInstance *inst, WlPresentBuffer *buffer)
{
    EGLBoolean success = EGL_FALSE;
    int fd = -1;

    assert(inst->supports_implicit_sync);

    fd = eplWlExportDmaBufSyncFile(buffer->dmabuf);
    if (fd >= 0)
    {
        success = WaitForSyncFDGPU(inst, fd);
    }

    if (success)
    {
        buffer->status = BUFFER_STATUS_IDLE;
    }

    return success;
}

/**
 * Waits or polls for a buffer to free up, using implicit sync.
 *
 * Note that we can only wait for a buffer if we've received a
 * wl_buffer::release event. If no buffers were ready, then the caller has to
 * wait for events and try again.
 *
 * \param inst The WlDisplayInstance pointer.
 * \param surf The EplSurface to wait on.
 * \param timeout_ms The number of milliseconds to wait. Zero to poll without blocking.
 *
 * \return The number of buffers that were checked, or -1 on error.
 */
static int CheckBufferReleaseImplicit(WlDisplayInstance *inst,
        WlSwapChain *swapchain, int timeout_ms)
{
    WlPresentBuffer *buffer;
    WlPresentBuffer **buffers;
    struct pollfd *fds;
    int count;
    int i;
    int ret;

    if (wl_display_dispatch_queue_pending(inst->wdpy, swapchain->queue) < 0)
    {
        return -1;
    }

    count = 0;
    glvnd_list_for_each_entry(buffer, &swapchain->present_buffers, entry)
    {
        if (buffer->status == BUFFER_STATUS_IDLE_NOTIFIED)
        {
            if (buffer->dmabuf >= 0 && inst->supports_implicit_sync)
            {
                if (WaitImplicitFence(inst, buffer))
                {
                    // If possible, extract a syncfd and wait on it using eglWaitSync,
                    // instead of doing a CPU wait.
                    assert(buffer->status == BUFFER_STATUS_IDLE);
                    return 1;
                }
                count++;
            }
            else
            {
                // If implicit sync isn't available at all, then just grab the
                // oldest buffer and hope for the best.
                buffer->status = BUFFER_STATUS_IDLE;
                return 1;
            }
        }
    }

    if (count == 0)
    {
        return 0;
    }

    // Sanity check: If implicit sync isn't available, then we should never
    // have incremented count above.
    assert(inst->supports_implicit_sync);

    buffers = alloca(count * sizeof(WlPresentBuffer *));
    fds = alloca(count * sizeof(struct pollfd));

    count = 0;
    glvnd_list_for_each_entry(buffer, &swapchain->present_buffers, entry)
    {
        if (buffer->status == BUFFER_STATUS_IDLE_NOTIFIED)
        {
            assert(buffer->dmabuf >= 0);

            buffers[count] = buffer;
            fds[count].fd = buffer->dmabuf;
            fds[count].events = POLLOUT;
            fds[count].revents = 0;
            count++;
        }
    }

    ret = poll(fds, count, timeout_ms);

    if (ret > 0)
    {
        for (i=0; i<count; i++)
        {
            if (fds[i].revents & POLLOUT)
            {
                buffers[i]->status = BUFFER_STATUS_IDLE;
            }
        }
        return count;
    }
    else if (ret == 0 || errno == ETIME || errno == EINTR)
    {
        // Nothing freed up before the timeout, but that's not a fatal error
        // here.
        return count;
    }
    else
    {
        eplSetError(inst->platform, EGL_BAD_ALLOC, "Internal error: poll() failed: %s\n",
                strerror(errno));
        return -1;
    }
}

WlPresentBuffer *eplWlSwapChainFindFreePresentBuffer(WlDisplayInstance *inst,
        WlSwapChain *swapchain)
{
    /*
     * First, poll to see if any buffers have already freed up. Do this up
     * front so that we don't try to allocate a new buffer unnecessarily.
     */
    if (inst->globals.syncobj != NULL)
    {
        if (CheckBufferReleaseExplicit(inst, swapchain, 0) < 0)
        {
            return NULL;
        }
    }
    else
    {
        if (CheckBufferReleaseImplicit(inst, swapchain, 0) < 0)
        {
            return NULL;
        }
    }

    while (1)
    {
        WlPresentBuffer *buf = NULL;
        size_t num_buffers = 0;
        glvnd_list_for_each_entry(buf, &swapchain->present_buffers, entry)
        {
            if (buf->status == BUFFER_STATUS_IDLE)
            {
                return buf;
            }
            num_buffers++;
        }

        if (num_buffers < MAX_PRESENT_BUFFERS)
        {
            // We didn't find a free buffer, but we don't have our maximum
            // number of buffers yet, so allocate a new one.
            return eplWlSwapChainCreatePresentBuffer(inst, swapchain);
        }

        // Otherwise, we have to wait for a buffer to free up.

        if (inst->globals.syncobj != NULL)
        {
            if (CheckBufferReleaseExplicit(inst, swapchain, -1) < 0)
            {
                return NULL;
            }
        }
        else
        {
            int numChecked = CheckBufferReleaseImplicit(inst, swapchain, RELEASE_WAIT_TIMEOUT);

            if (numChecked < 0)
            {
                return NULL;
            }
            else if (numChecked == 0)
            {
                /*
                 * There weren't any buffers to wait on yet, so wait for a
                 * wl_surface::release event.
                 *
                 * If we receieve a release event, then the handler will mark
                 * the corresponding buffer as ready to wait on, and then
                 * CheckBufferReleaseImplicit will find it on the next pass
                 * through this loop.
                 */
                if (wl_display_dispatch_queue(inst->wdpy, swapchain->queue) < 0)
                {
                    return NULL;
                }
            }
        }
    }
}
