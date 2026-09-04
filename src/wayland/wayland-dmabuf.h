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

#include "linux-dmabuf-v1-client-protocol.h"

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
 * Keeps track of feedback data from a \c zwp_linux_dmabuf_feedback_v1.
 *
 * This object keeps track of all of the dma-buf feedback data as it arrives.
 *
 * When we get a \c zwp_linux_dmabuf_feedback_v1::done event, we compile the
 * data into a list of \c WlDmaBufFeedbackTranche structs, and pass them to a
 * callback function.
 *
 * Internally, this will handle different versions of the dma-buf protocol, and
 * will translate data from older versions to the equivalent data in version 6.
 *
 * However, versions 3 and lower don't use the feedback protocol at all, so that
 * will require special handling for getting the default feedback.
 */
typedef struct WlDmaBufFeedbackRec WlDmaBufFeedback;

/**
 * Data for each tranche of dma-buf feedback.
 */
typedef struct
{
    /**
     * The target device.
     */
    dev_t target_device;

    /**
     * The tranche flags.
     *
     * This will include the SAMPLING flag, even on older version of the
     * dma-buf protocol. For v4 and v5, we'll set the SAMPLING flag based on
     * the \c main_device event.
     */
    uint32_t flags;

    /**
     * The set of formats and modifiers that this tranche listed.
     */
    WlFormatList *formats;

    struct glvnd_list entry;
} WlDmaBufFeedbackTranche;

/**
 * A callback function to handle a new batch of dma-buf feedback.
 *
 * After the callback, all elements in \p tranches will be freed. If the
 * callback wants to store the tranche data, then it can remove any entries it
 * needs from the list before returning.
 *
 * \param feedback The WlDmaBufFeedback struct
 * \param tranches A list of \c WlDmaBufFeedbackTranche structs
 * \param param The callback parameter passed to \c eplWlDmaBufFeedbackInit.
 */
typedef void (* WlDmaBufFeedbackCallback) (WlDmaBufFeedback *feedback,
        struct glvnd_list *tranches, void *param);

/**
 * Sets up a listener to process dma-buf feedback.
 *
 * \param plat The platform struct.
 * \param wfeedback The feedback proxy.
 * \param callback The callback function to call after each complete batch of
 *      feedback.
 * \param param A parameter to pass through to \p callback.
 * \return A new WlDmaBufFeedback pointer, or NULL on failure.
 */
WlDmaBufFeedback *eplWlDmaBufFeedbackInit(EplPlatformData *plat,
        struct zwp_linux_dmabuf_feedback_v1 *wfeedback,
        WlDmaBufFeedbackCallback callback, void *param);

/**
 * Cleans up a WlDmaBufFeedback.
 *
 * \param feedback The feedback struct to clean up.
 * \param display_valid True if the wl_display is still valid.
 */
void eplWlDmaBufFeedbackDestroy(WlDmaBufFeedback *feedback);

/**
 * Frees a single WlDmaBufFeedbackTranche object.
 */
void eplWlDmaBufFeedbackTrancheFree(WlDmaBufFeedbackTranche *tranche);

/**
 * Frees a list of WlDmaBufFeedbackTranche objects.
 */
void eplWlDmaBufFeedbackTrancheFreeList(struct glvnd_list *tranches);

/**
 * Converts an array of WlDmaBufFeedbackTableEntry into a WlFormatList.
 */
WlFormatList *eplWlCompileFormatList(const WlDmaBufFeedbackTableEntry *format_entries, size_t count);

/**
 * Figures out which modifiers are supported by a single dma-buf feedback
 * tranche.
 *
 * \param tranche The tranche to check.
 * \param render_devices The dev_t values for the device that will export the
 *      dma-bufs.
 * \param render_device_count The number of elements in \p render_devices.
 * \param fourcc The fourcc format code to check.
 * \param driver_mods The set of modifiers to check. This should be the set
 *      of modifiers that the driver supports for rendering.
 * \param num_driver_mods The number of elements in the \p driver_mods and
 *      \p ret_supported_mods arrays.
 * \param[out] ret_supported_mods Returns the subset of \p driver_mods that the
 *      server supports.
 * \param[out] ret_supports_linear Returns true if the server can accept a
 *      pitch linear buffer.
 *
 * \return The number of modifiers in \p driver_mods that the server supports.
 *      If none of them are supported, but the server supports pitch linear,
 *      then returns zero. If the server doesn't support any modifiers, and
 *      doesn't support pitch linear, then returns -1.
 */
ssize_t eplWlDmaBufGetSupportedTrancheModifiers(
        const WlDmaBufFeedbackTranche *tranche,
        const dev_t *render_devices,
        size_t render_device_count,
        uint32_t fourcc,
        const uint64_t *driver_mods,
        size_t num_driver_mods,
        uint64_t *ret_supported_mods,
        EGLBoolean *ret_supports_linear);

/**
 * Figures out which modifiers are supported by the server.
 *
 * This is just a wrapper around eplWlDmaBufGetSupportedTrancheModifiers, which
 * returns the first tranche that supports anything.
 *
 * \param tranches A linked list of WlDmaBufFeedbackTranche structs.
 * \param render_devices The dev_t values for the device that will export the
 *      dma-bufs.
 * \param render_device_count The number of elements in \p render_devices.
 * \param fourcc The fourcc format code to check.
 * \param driver_mods The set of modifiers to check. This should be the set
 *      of modifiers that the driver supports for rendering.
 * \param num_driver_mods The number of elements in the \p driver_mods and
 *      \p ret_supported_mods arrays.
 * \param[out] ret_supported_mods Returns the subset of \p driver_mods that the
 *      server supports.
 * \param[out] ret_supports_linear Returns true if the server can accept a
 *      pitch linear buffer.
 * \param[out] ret_sampling_device Returns the dev_t for the node that should
 *      be set as the sampling device.
 *
 * \return The number of modifiers in \p driver_mods that the server supports.
 *      If none of them are supported, but the server supports pitch linear,
 *      then returns zero. If the server doesn't support any modifiers, and
 *      doesn't support pitch linear, then returns -1.
 */
ssize_t eplWlDmaBufGetSupportedModifiers(struct glvnd_list *tranches,
        const dev_t *render_devices,
        size_t render_device_count,
        uint32_t fourcc,
        const uint64_t *driver_mods,
        size_t num_driver_mods,
        uint64_t *ret_supported_mods,
        EGLBoolean *ret_supports_linear,
        dev_t *ret_sampling_device);

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
