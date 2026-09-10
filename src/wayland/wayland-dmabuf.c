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

#include "wayland-dmabuf.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <assert.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include "wayland-platform.h"

struct WlDmaBufFeedbackRec
{
    EplPlatformData *plat;

    WlDmaBufFeedbackCallback callback;
    void *callback_param;

    /// The current format table.
    WlDmaBufFeedbackTableEntry *format_table;
    size_t format_table_len;

    /// The dev_t from the main_device event.
    dev_t main_device;

    /// True if we've received a main_device event in this batch.
    EGLBoolean got_main_device;

    /// The target device for the current tranche.
    dev_t tranche_target_device;

    /// True if we've received a tranche_target_device event in the current tranche.
    EGLBoolean got_tranche_target_device;

    /// The flags for the current tranche.
    uint32_t tranche_flags;

    /**
     * An array of WlDmaBufFeedbackTableEntry elements for all of the
     * format/modifier pairs that we got for this tranche.
     */
    struct wl_array tranche_formats;

    /**
     * A linked list of the WlDmaBufFeedbackTranche structs for all the
     * tranches that we've received so far.
     */
    struct glvnd_list tranches;

    /// If true, then we ran into a malloc failure or some other error along the way.
    EGLBoolean error;
};

int eplWlCompareU32(const void *p1, const void *p2)
{
    uint32_t v1 = *((const uint32_t *) p1);
    uint32_t v2 = *((const uint32_t *) p2);
    if (v1 < v2)
    {
        return -1;
    }
    else if (v1 > v2)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

void eplWlFormatListFree(WlFormatList *data)
{
    // We allocate everything in one block, so just free it.
    free(data);
}

const WlDmaBufFormat *eplWlDmaBufFormatFind(const WlDmaBufFormat *formats, size_t count, uint32_t fourcc)
{
    return bsearch(&fourcc, formats, count, sizeof(WlDmaBufFormat), eplWlCompareU32);
}

EGLBoolean eplWlDmaBufFormatSupportsModifier(const WlDmaBufFormat *format, uint64_t modifier)
{
    size_t i;
    for (i=0; i<format->num_modifiers; i++)
    {
        if (format->modifiers[i] == modifier)
        {
            return EGL_TRUE;
        }
    }
    return EGL_FALSE;
}

WlFormatList *eplWlCompileFormatList(const WlDmaBufFeedbackTableEntry *format_entries, size_t num_entries)
{
    WlFormatList *formats = NULL;
    struct wl_array fourccs;
    size_t num_formats = 0;
    uint32_t *fourcc;
    uint64_t *mods_ptr;
    size_t i;

    // Sanity check: We should have at least one format and modifier.
    assert(num_entries > 0);

    wl_array_init(&fourccs);

    // Find the set of fourcc codes in this tranche.
    for (i=0; i<num_entries; i++)
    {
        EGLBoolean found = EGL_FALSE;

        wl_array_for_each(fourcc, &fourccs)
        {
            if (*fourcc == format_entries[i].fourcc)
            {
                found = EGL_TRUE;
                break;
            }
        }
        if (!found)
        {
            fourcc = wl_array_add(&fourccs, sizeof(uint32_t));
            if (fourcc == NULL)
            {
                goto done;
            }
            *fourcc = format_entries[i].fourcc;
        }
    }

    num_formats = fourccs.size / sizeof(uint32_t);
    qsort(fourccs.data, num_formats, sizeof(uint32_t), eplWlCompareU32);

    // Allocate enough space for the WlFormatList itself, plus all
    // of the format structs, plus all of the modifier lists.
    formats = malloc(sizeof(WlFormatList)
            + num_formats * sizeof(WlDmaBufFormat)
            + num_entries * sizeof(uint64_t));
    if (formats == NULL)
    {
        goto done;
    }
    formats->formats = (WlDmaBufFormat *) (formats + 1);
    mods_ptr = (uint64_t *) (formats->formats + num_formats);

    formats->num_formats = 0;
    wl_array_for_each(fourcc, &fourccs)
    {
        const EplFormatInfo *info = eplFormatInfoLookup(*fourcc);
        if (info != NULL)
        {
            WlDmaBufFormat *fmt = &formats->formats[formats->num_formats++];

            fmt->fourcc = *fourcc;
            fmt->fmt = info;
            fmt->modifiers = mods_ptr;
            fmt->num_modifiers = 0;

            for (i=0; i<num_entries; i++)
            {
                if (format_entries[i].fourcc == *fourcc)
                {
                    fmt->modifiers[fmt->num_modifiers++] = format_entries[i].modifier;
                }
            }
            mods_ptr += fmt->num_modifiers;
        }
    }

done:
    wl_array_release(&fourccs);
    return formats;
}

static void on_feedback_done(void *userdata, struct zwp_linux_dmabuf_feedback_v1 *wfeedback)
{
    WlDmaBufFeedback *feedback = userdata;

    if (wl_proxy_get_version((struct wl_proxy *) wfeedback)
            < ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SAMPLING_SINCE_VERSION)
    {
        /*
         * If we're using version <= 5, then set the sampling flags based on
         * the main device event.
         */
        if (feedback->got_main_device)
        {
            WlDmaBufFeedbackTranche *tranche;
            glvnd_list_for_each_entry(tranche, &feedback->tranches, entry)
            {
                if (tranche->target_device == feedback->main_device)
                {
                    tranche->flags |= ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SAMPLING;
                }
            }
        }
        else
        {
            feedback->error = EGL_TRUE;
        }
    }

    if (!feedback->error && !glvnd_list_is_empty(&feedback->tranches))
    {
        feedback->callback(feedback, &feedback->tranches, feedback->callback_param);
    }

    // After the callback, clean up the list and prepare for the next batch.
    eplWlDmaBufFeedbackTrancheFreeList(&feedback->tranches);
    feedback->error = EGL_FALSE;
    feedback->main_device = 0;
    feedback->got_main_device = EGL_FALSE;
}


static void on_feedback_format_table(void *userdata,
        struct zwp_linux_dmabuf_feedback_v1 *wfeedback,
        int32_t fd, uint32_t size)
{
    WlDmaBufFeedback *feedback = userdata;
    size_t len;

    if (feedback->format_table != NULL)
    {
        munmap(feedback->format_table, feedback->format_table_len * sizeof(WlDmaBufFeedbackTableEntry));
        feedback->format_table = NULL;
        feedback->format_table_len = 0;
    }

    len = size / sizeof(WlDmaBufFeedbackTableEntry);
    if (len > 0)
    {
        void *ptr = mmap(NULL, len * sizeof(WlDmaBufFeedbackTableEntry),
                PROT_READ, MAP_PRIVATE, fd, 0);
        if (ptr != MAP_FAILED)
        {
            feedback->format_table = ptr;
            feedback->format_table_len = len;
        }
        else
        {
            feedback->plat->callbacks.debugMessage(EGL_DEBUG_MSG_WARN_KHR,
                    "mmap failed when for dma-buf feedback format table");
            feedback->error = EGL_TRUE;
        }
    }

    close(fd);
}

static void on_feedback_main_device(void *userdata,
            struct zwp_linux_dmabuf_feedback_v1 *wfeedback,
            struct wl_array *device)
{
    WlDmaBufFeedback *feedback = userdata;
    if (device->size >= sizeof(dev_t))
    {
        memcpy(&feedback->main_device, device->data, sizeof(dev_t));
        feedback->got_main_device = EGL_TRUE;
    }
}

static void on_feedback_tranche_done(void *userdata,
             struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1)
{
    WlDmaBufFeedback *feedback = userdata;
    WlDmaBufFeedbackTranche *tranche = NULL;
    size_t num_entries;

    if (feedback->error)
    {
        goto done;
    }

    // Make sure the tranche data looks valid.
    num_entries = feedback->tranche_formats.size / sizeof(WlDmaBufFeedbackTableEntry);
    if (num_entries <= 0)
    {
        goto done;
    }
    if (!feedback->got_tranche_target_device)
    {
        goto done;
    }

    tranche = calloc(1, sizeof(WlDmaBufFeedbackTranche));
    if (tranche == NULL)
    {
        feedback->plat->callbacks.debugMessage(EGL_DEBUG_MSG_WARN_KHR,
                "Out of memory processing dma-buf feedback");
        feedback->error = EGL_TRUE;
        goto done;
    }
    glvnd_list_init(&tranche->entry);

    tranche->formats = eplWlCompileFormatList(feedback->tranche_formats.data, num_entries);
    if (tranche->formats == NULL)
    {
        free(tranche);
        tranche = NULL;
        feedback->plat->callbacks.debugMessage(EGL_DEBUG_MSG_WARN_KHR,
                "Out of memory processing dma-buf feedback");
        feedback->error = EGL_TRUE;
        goto done;
    }

    tranche->flags = feedback->tranche_flags;
    tranche->target_device = feedback->tranche_target_device;
    glvnd_list_append(&tranche->entry, &feedback->tranches);

done:
    feedback->tranche_target_device = 0;
    feedback->got_tranche_target_device = EGL_FALSE;
    feedback->tranche_flags = 0;
    wl_array_release(&feedback->tranche_formats);
    wl_array_init(&feedback->tranche_formats);
}

static void on_feedback_tranche_target_device(void *userdata,
                  struct zwp_linux_dmabuf_feedback_v1 *wfeedback,
                  struct wl_array *device)
{
    WlDmaBufFeedback *feedback = userdata;
    if (device->size >= sizeof(dev_t))
    {
        memcpy(&feedback->tranche_target_device, device->data, sizeof(dev_t));
        feedback->got_tranche_target_device = EGL_TRUE;
    }
}

static void on_feedback_tranche_formats(void *userdata,
            struct zwp_linux_dmabuf_feedback_v1 *wfeedback,
            struct wl_array *indices)
{
    WlDmaBufFeedback *feedback = userdata;
    uint16_t *index;

    if (feedback->error || indices->size < sizeof(uint64_t))
    {
        return;
    }
    else if (feedback->format_table == NULL)
    {
        feedback->plat->callbacks.debugMessage(EGL_DEBUG_MSG_WARN_KHR,
                "Got zwp_linux_dmabuf_feedback_v1::tranche_formats with no format table");
        feedback->error = EGL_TRUE;
        return;
    }

    wl_array_for_each(index, indices)
    {
        const WlDmaBufFeedbackTableEntry *src;
        WlDmaBufFeedbackTableEntry *entry;
        EGLBoolean found = EGL_FALSE;

        if (*index >= feedback->format_table_len)
        {
            continue;
        }

        src = &feedback->format_table[*index];
        if (src->fourcc == DRM_FORMAT_INVALID || src->modifier == DRM_FORMAT_MOD_INVALID)
        {
            continue;
        }

        if (eplFormatInfoLookup(src->fourcc) == NULL)
        {
            // Skip any formats that we don't recognize.
            continue;
        }

        // Skip if this (format, modifier) pair is already in our list.
        wl_array_for_each(entry, &feedback->tranche_formats)
        {
            if (entry->fourcc == src->fourcc
                    && entry->modifier == src->modifier)
            {
                found = EGL_TRUE;
                break;
            }
        }
        if (found)
        {
            continue;
        }

        entry = wl_array_add(&feedback->tranche_formats, sizeof(WlDmaBufFeedbackTableEntry));
        if (entry == NULL)
        {
            feedback->plat->callbacks.debugMessage(EGL_DEBUG_MSG_WARN_KHR,
                    "Out of memory processing dma-buf feedback");
            feedback->error = EGL_TRUE;
            break;
        }

        *entry = *src;
    }
}

static void on_feedback_tranche_flags(void *userdata,
        struct zwp_linux_dmabuf_feedback_v1 *wfeedback,
        uint32_t flags)
{
    WlDmaBufFeedback *feedback = userdata;
    feedback->tranche_flags = flags;
}

static const struct zwp_linux_dmabuf_feedback_v1_listener DMABUF_FEEDBACK_LISTENER =
{
	.done = on_feedback_done,
	.format_table = on_feedback_format_table,
	.main_device = on_feedback_main_device,
	.tranche_done = on_feedback_tranche_done,
	.tranche_target_device = on_feedback_tranche_target_device,
	.tranche_formats = on_feedback_tranche_formats,
	.tranche_flags = on_feedback_tranche_flags,
};

WlDmaBufFeedback *eplWlDmaBufFeedbackInit(EplPlatformData *plat,
        struct zwp_linux_dmabuf_feedback_v1 *wfeedback,
        WlDmaBufFeedbackCallback callback, void *param)
{
    WlDmaBufFeedback *feedback = calloc(1, sizeof(WlDmaBufFeedback));
    if (feedback == NULL)
    {
        return NULL;
    }

    wl_array_init(&feedback->tranche_formats);
    glvnd_list_init(&feedback->tranches);

    feedback->plat = eplPlatformDataRef(plat);
    feedback->callback = callback;
    feedback->callback_param = param;
    zwp_linux_dmabuf_feedback_v1_add_listener(wfeedback, &DMABUF_FEEDBACK_LISTENER, feedback);

    return feedback;
}

void eplWlDmaBufFeedbackDestroy(WlDmaBufFeedback *feedback)
{
    if (feedback != NULL)
    {
        if (feedback->format_table != NULL)
        {
            munmap(feedback->format_table, feedback->format_table_len);
        }
        eplPlatformDataUnref(feedback->plat);

        wl_array_release(&feedback->tranche_formats);

        eplWlDmaBufFeedbackTrancheFreeList(&feedback->tranches);

        free(feedback);
    }
}

void eplWlDmaBufFeedbackTrancheFree(WlDmaBufFeedbackTranche *tranche)
{
    if (tranche != NULL)
    {
        free(tranche->formats);
        free(tranche);
    }
}

void eplWlDmaBufFeedbackTrancheFreeList(struct glvnd_list *tranches)
{
    while (!glvnd_list_is_empty(tranches))
    {
        WlDmaBufFeedbackTranche *tranche = glvnd_list_first_entry(tranches, WlDmaBufFeedbackTranche, entry);
        glvnd_list_del(&tranche->entry);
        eplWlDmaBufFeedbackTrancheFree(tranche);
    }
}

ssize_t eplWlDmaBufGetSupportedTrancheModifiers(
        const WlDmaBufFeedbackTranche *tranche,
        const WlDeviceIds *render_device,
        uint32_t fourcc,
        const uint64_t *driver_mods,
        size_t num_driver_mods,
        uint64_t *ret_supported_mods,
        EGLBoolean *ret_supports_linear)
{
    const WlDmaBufFormat *fmt = eplWlDmaBufFormatFind(tranche->formats->formats,
            tranche->formats->num_formats, fourcc);
    size_t num_supported = 0;
    EGLBoolean supports_linear;

    if (fmt == NULL)
    {
        return -1;
    }

    if (tranche->flags & ZWP_LINUX_DMABUF_FEEDBACK_V1_TRANCHE_FLAGS_SAMPLING)
    {
        if (eplWlCheckDeviceId(render_device, tranche->target_device))
        {
            size_t i;

            // The server can sample directly from the rendering device, so see
            // which modifiers the server can handle.
            for (i=0; i<num_driver_mods; i++)
            {
                if (eplWlDmaBufFormatSupportsModifier(fmt, driver_mods[i]))
                {
                    if (ret_supported_mods != NULL)
                    {
                        ret_supported_mods[num_supported] = driver_mods[i];
                    }
                    num_supported++;
                }
            }
        }

        // For PRIME, we use a buffer in sysmem, so any sampling device should
        // be able to read it.
        supports_linear = eplWlDmaBufFormatSupportsModifier(fmt, DRM_FORMAT_MOD_LINEAR);
    }

    if (num_supported > 0 || supports_linear)
    {
        if (ret_supports_linear != NULL)
        {
            *ret_supports_linear = supports_linear;
        }
        return num_supported;
    }
    else
    {
        return -1;
    }
}

ssize_t eplWlDmaBufGetSupportedModifiers(struct glvnd_list *tranches,
        const WlDeviceIds *render_device,
        uint32_t fourcc,
        const uint64_t *driver_mods,
        size_t num_driver_mods,
        uint64_t *ret_supported_mods,
        EGLBoolean *ret_supports_linear,
        dev_t *ret_sampling_device)
{
    const WlDmaBufFeedbackTranche *tranche;

    glvnd_list_for_each_entry(tranche, tranches, entry)
    {
        ssize_t ret = eplWlDmaBufGetSupportedTrancheModifiers(tranche,
                render_device, fourcc, driver_mods, num_driver_mods,
                ret_supported_mods, ret_supports_linear);
        if (ret >= 0)
        {
            if (ret_sampling_device != NULL)
            {
                *ret_sampling_device = tranche->target_device;
            }
            return ret;
        }
    }
    return -1;
}
