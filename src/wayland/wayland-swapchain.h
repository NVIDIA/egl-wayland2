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

#ifndef WAYLAND_SWAPCHAIN_H
#define WAYLAND_SWAPCHAIN_H

/**
 * \file
 *
 * Functions for keeping track of the color buffers for a surface.
 */

#include <stdlib.h>

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include "wayland-platform.h"
#include "wayland-timeline.h"

typedef enum
{
    /**
     * The buffer is idle, so we can use it immediately.
     */
    BUFFER_STATUS_IDLE,

    /**
     * The buffer is in use in the server, and we have not yet received a
     * wl_buffer::release event for it.
     */
    BUFFER_STATUS_IN_USE,

    /**
     * We've received a wl_buffer::release event for this buffer, but we
     * haven't waited for it to actually be free yet.
     *
     * This is only used with implicit sync.
     */
    BUFFER_STATUS_IDLE_NOTIFIED,
} WlBufferStatus;

/**
 * A shared color buffer that we can use for presentation.
 *
 * Note that under PRIME, this will be a linear buffer, not one that we can
 * render to.
 */
typedef struct
{
    /**
     * The handle for the color buffer in the driver.
     */
    EGLPlatformColorBufferNVX buffer;

    /**
     * Whether this buffer is still in use by the server.
     */
    WlBufferStatus status;

    /**
     * The wl_buffer object for this buffer.
     */
    struct wl_buffer *wbuf;

    /**
     * A file descriptor for the dma-buf.
     *
     * Note that currently, this is only used for implicit sync.
     */
    int dmabuf;

    /**
     * A timeline sync object.
     *
     * It's possible that different buffers could go through a different
     * presentation path in the server, which could in turn cause them to be
     * released in a different order than they were presented.
     *
     * To cope with that, we give each buffer its own timeline for acquire and
     * release points.
     */
    WlTimeline timeline;

    struct glvnd_list entry;
} WlPresentBuffer;

/**
 * Keeps track of a set of color buffers for a surface.
 */
typedef struct
{
    /**
     * The size of the buffers.
     */
    EGLint width;
    EGLint height;

    uint32_t fourcc;

    /**
     * The format modifier that we're using for the present buffers.
     *
     * For PRIME, this is currently always DRM_FORMAT_MOD_LINEAR. If/when we
     * have a way to do cross-device blits with different layouts, though, then
     * this may be some other modifier that the server understands.
     *
     * For non-PRIME, this is the format modifier of the renderable buffers,
     * since those are also the present buffers.
     */
    uint64_t modifier;

    /**
     * True if we're presenting using PRIME.
     */
    EGLBoolean prime;

    /**
     * The color buffers that we've allocated for this window.
     *
     * This is a list of WlPresentBuffer structs.
     *
     * For PRIME, these will be linear buffers, not renderable buffers.
     */
    struct glvnd_list present_buffers;

    /**
     * A pointer to the current back buffer.
     *
     * This is NULL if we're using PRIME.
     */
    WlPresentBuffer *current_back;

    /**
     * The current buffer that we're rendering to.
     *
     * For PRIME, this will be a single, fixed buffer.
     *
     * For non-PRIME, this will be the same buffer as \c current_back.
     */
    EGLPlatformColorBufferNVX render_buffer;
} WlSwapChain;

/**
 * Creates a swapchain, with an initial renderable buffer.
 *
 * \param inst The WlDisplayInstance for the display
 * \param queue The surface's event queue
 * \param width The width of the surface
 * \param height The height of the surface
 * \param fourcc The fourcc format code
 * \param prime True to use PRIME for presentation, or false if we can present
 *      directly.
 * \param modifiers A list of allowed modifiers for the buffers.
 * \param num_modifiers The number of modifiers in \c modifiers.
 */
WlSwapChain *eplWlSwapChainCreate(WlDisplayInstance *inst,
        struct wl_event_queue *queue, uint32_t width, uint32_t height,
        uint32_t fourcc, EGLBoolean prime,
        const uint64_t *modifiers, size_t num_modifiers);

void eplWlSwapChainDestroy(WlDisplayInstance *inst, WlSwapChain *swapchain);

WlPresentBuffer *eplWlSwapChainCreatePresentBuffer(WlDisplayInstance *inst,
        WlSwapChain *swapchain, struct wl_event_queue *queue);

/**
 * Returns a free present buffer.
 *
 * If there isn't a free buffer, then this will either allocate a new one, or
 * wait for one to free up.
 */
WlPresentBuffer *eplWlSwapChainFindFreePresentBuffer(WlDisplayInstance *inst,
        WlSwapChain *swapchain, struct wl_event_queue *queue);

#endif // WAYLAND_SWAPCHAIN_H
