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

#ifndef WAYLAND_DMABUF_H
#define WAYLAND_DMABUF_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <assert.h>

#include <drm_fourcc.h>
#include <wayland-client-core.h>

#include "linux-dmabuf-unstable-v1-client-protocol.h"

#include "wayland-platform.h"
#include "config-list.h"

/**
 * Keeps track of format and format modifier support.
 *
 * We use this struct for keeping track of both driver support (from
 * eglQueryDmaBufFormatsEXT and eglQueryDmaBufModifiersEXT) and for server
 * support.
 */
typedef struct _WlDmaBufFormat
{
    // Put the fourcc code as the first element so that we can use bsearch
    // with just the fourcc code for a key.
    uint32_t fourcc;
    const EplFormatInfo *fmt;
    uint64_t *modifiers;
    size_t num_modifiers;
} WlDmaBufFormat;

/**
 * Contains a list of formats, with the supported modifiers for each.
 */
typedef struct _WlFormatList
{
    /// An array of supported formats, sorted by fourcc code.
    WlDmaBufFormat *formats;
    size_t num_formats;
} WlFormatList;

/**
 * An entry in the mmap'ed format table for a dma-buf feedback object.
 */
typedef struct
{
    uint32_t fourcc;
    uint32_t pad;
    uint64_t modifier;
} WlDmaBufFeedbackTableEntry;

/**
 * A common struct for handling dma-buf feedback data.
 *
 * This is used by the common handlers for default and per-surface feedback.
 */
typedef struct
{
    /// The current format table.
    WlDmaBufFeedbackTableEntry *format_table;
    size_t format_table_len;

    dev_t main_device;

    /// The target device for the current tranche.
    dev_t tranche_target_device;

    /// The flags for the current tranche.
    uint32_t tranche_flags;

    /// If true, then we ran into a malloc failure or some other error along the way.
    EGLBoolean error;
} WlDmaBufFeedbackCommon;

void eplWlDmaBufFeedbackCommonInit(WlDmaBufFeedbackCommon *base);
void eplWlDmaBufFeedbackCommonCleanup(WlDmaBufFeedbackCommon *base);

/**
 * Called for a zwp_linux_dmabuf_feedback_v1::done event.
 *
 * This just clears any data to get ready for the next update, so it should be
 * called after the caller processes whatever data is there.
 */
void eplWlDmaBufFeedbackCommonDone(WlDmaBufFeedbackCommon *base);

/**
 * Called for a zwp_linux_dmabuf_feedback_v1::tranche_done event.
 *
 * This just clears any data to get ready for the next tranche, so it should be
 * called after the caller processes whatever data is there.
 */
void eplWlDmaBufFeedbackCommonTrancheDone(WlDmaBufFeedbackCommon *base);

/**
 * Handles a zwp_linux_dmabuf_feedback_v1::format_table event.
 */
void eplWlDmaBufFeedbackCommonFormatTable(void *userdata,
        struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
        int32_t fd, uint32_t size);

void eplWlDmaBufFeedbackCommonMainDevice(void *userdata,
        struct zwp_linux_dmabuf_feedback_v1 *wfeedback,
        struct wl_array *device);

void eplWlDmaBufFeedbackCommonTrancheTargetDevice(void *userdata,
        struct zwp_linux_dmabuf_feedback_v1 *wfeedback,
        struct wl_array *device);

void eplWlDmaBufFeedbackCommonTrancheFlags(void *userdata,
        struct zwp_linux_dmabuf_feedback_v1 *wfeedback,
        uint32_t flags);

/**
 * Returns the default dma-buf feedback data.
 *
 * If the \c zwp_linux_dmabuf_v1 is version 3, then this will instead use the old
 * events on the \c zwp_linux_dmabuf_v1 itself to get a format and modifier
 * list. In that case, it will return zero for \p main_device fields will be
 * zero.
 *
 * For version 4 or later, this will use a \c zwp_linux_dmabuf_feedback_v1 to
 * get the default feedback data. It will return a combined format list for all
 * of the tranches for the main device, and ignore any tranches that apply to
 * any other devices.
 *
 * \param wdpy The display connection
 * \param wdmabuf The \c zwp_linux_dmabuf_v1 proxy
 * \param queue The event queue associated with \p wdmabuf. Note that this is
 *      only used for version 3. For version 4, the zwp_linux_dmabuf_feedback_v1
 *      gets its own event queue.
 * \return A new \c WlFormatList object, or NULL on error. The caller
 *      must free it using \c eplWlFormatListFree.
 */
WlFormatList *eplWlDmaBufFeedbackGetDefault(struct wl_display *wdpy,
        struct zwp_linux_dmabuf_v1 *wdmabuf,
        struct wl_event_queue *queue,
        dev_t *ret_main_device);

void eplWlFormatListFree(WlFormatList *data);

const WlDmaBufFormat *eplWlDmaBufFormatFind(const WlDmaBufFormat *formats,
        size_t count, uint32_t fourcc);

EGLBoolean eplWlDmaBufFormatSupportsModifier(const WlDmaBufFormat *format, uint64_t modifier);

/**
 * A comparison function for \c bsearch or \c qsort which sorts based on a
 * uint32_t.
 *
 * TODO: Move this into the base library?
 */
int eplWlCompareU32(const void *p1, const void *p2);

#endif // WAYLAND_DMABUF_H
