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

typedef struct
{
    dev_t target_device;
    uint32_t flags;

    /**
     * An array of WlDmaBufFeedbackTableEntry elements for all of the
     * format/modifier pairs that we got for this tranche.
     */
    struct wl_array formats;

    struct glvnd_list entry;
} DefaultFeedbackTranche;

typedef struct
{
    WlDmaBufFeedbackCommon base;

    struct wl_array tranche_formats;
    struct glvnd_list tranches;
    EGLBoolean done;
} DefaultFeedbackState;

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

void eplWlDmaBufFeedbackCommonInit(WlDmaBufFeedbackCommon *base)
{
    memset(base, 0, sizeof(*base));
}

void eplWlDmaBufFeedbackCommonCleanup(WlDmaBufFeedbackCommon *base)
{
    if (base->format_table != NULL)
    {
        munmap(base->format_table, base->format_table_len * sizeof(WlDmaBufFeedbackTableEntry));
        base->format_table = NULL;
        base->format_table_len = 0;
    }
}

void eplWlDmaBufFeedbackCommonDone(WlDmaBufFeedbackCommon *base)
{
    eplWlDmaBufFeedbackCommonTrancheDone(base);
    base->error = EGL_FALSE;
}

void eplWlDmaBufFeedbackCommonTrancheDone(WlDmaBufFeedbackCommon *base)
{
    base->tranche_target_device = 0;
    base->tranche_flags = 0;
}

void eplWlDmaBufFeedbackCommonFormatTable(void *userdata,
        struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
        int32_t fd, uint32_t size)
{
    WlDmaBufFeedbackCommon *base = userdata;
    size_t len;

    if (base->format_table != NULL)
    {
        munmap(base->format_table, base->format_table_len * sizeof(WlDmaBufFeedbackTableEntry));
        base->format_table = NULL;
        base->format_table_len = 0;
    }

    len = size / sizeof(WlDmaBufFeedbackTableEntry);
    if (len > 0)
    {
        void *ptr = mmap(NULL, len * sizeof(WlDmaBufFeedbackTableEntry),
                PROT_READ, MAP_PRIVATE, fd, 0);
        if (ptr != MAP_FAILED)
        {
            base->format_table = ptr;
            base->format_table_len = len;
        }
        else
        {
            base->error = EGL_TRUE;
        }
    }

    close(fd);
}

void eplWlDmaBufFeedbackCommonMainDevice(void *userdata,
        struct zwp_linux_dmabuf_feedback_v1 *wfeedback,
        struct wl_array *device)
{
    WlDmaBufFeedbackCommon *base = userdata;
    if (device->size >= sizeof(dev_t))
    {
        memcpy(&base->main_device, device->data, sizeof(dev_t));
    }
}

void eplWlDmaBufFeedbackCommonTrancheTargetDevice(void *userdata,
        struct zwp_linux_dmabuf_feedback_v1 *wfeedback,
        struct wl_array *device)
{
    WlDmaBufFeedbackCommon *base = userdata;
    if (device->size >= sizeof(dev_t))
    {
        memcpy(&base->tranche_target_device, device->data, sizeof(dev_t));
    }
}

void eplWlDmaBufFeedbackCommonTrancheFlags(void *userdata,
              struct zwp_linux_dmabuf_feedback_v1 *wfeedback,
              uint32_t flags)
{
    WlDmaBufFeedbackCommon *base = userdata;
    base->tranche_flags = flags;
}

static WlFormatList *FinishDefaultFeedback(DefaultFeedbackState *state, dev_t *ret_main_device)
{
    WlFormatList *data = NULL;
    struct wl_array fourccs;
    struct wl_array modifiers;
    DefaultFeedbackTranche *tranche;
    size_t num_formats = 0;
    size_t num_modifiers = 0;
    uint64_t *mods_dst;
    uint32_t *fourcc;

    wl_array_init(&fourccs);
    wl_array_init(&modifiers);

    if (state->base.error)
    {
        goto done;
    }

    // Build a list of fourcc codes, and a list of (fourcc, modifier) pairs
    // from all of the tranches that we care about, without any duplicates.
    glvnd_list_for_each_entry(tranche, &state->tranches, entry)
    {
        const WlDmaBufFeedbackTableEntry *src;

        if (tranche->target_device != state->base.main_device)
        {
            wl_array_release(&tranche->formats);
            wl_array_init(&tranche->formats);
            continue;
        }

        wl_array_for_each(src, &tranche->formats)
        {
            EGLBoolean found = EGL_FALSE;
            WlDmaBufFeedbackTableEntry *mod;

            if (eplFormatInfoLookup(src->fourcc) == NULL)
            {
                // Skip any formats that we don't recognize.
                // TODO: Would it be better to do this in the event callbacks?
                continue;
            }

            wl_array_for_each(fourcc, &fourccs)
            {
                if (*fourcc == src->fourcc)
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
                *fourcc = src->fourcc;
            }

            found = EGL_FALSE;
            wl_array_for_each(mod, &modifiers)
            {
                if (mod->fourcc == src->fourcc && mod->modifier == src->modifier)
                {
                    found = EGL_TRUE;
                    break;
                }
            }
            if (!found)
            {
                mod = wl_array_add(&modifiers, sizeof(WlDmaBufFeedbackTableEntry));
                if (mod == NULL)
                {
                    goto done;
                }
                *mod = *src;
            }
        }
    }

    num_formats = fourccs.size / sizeof(uint32_t);
    num_modifiers = modifiers.size / sizeof(WlDmaBufFeedbackTableEntry);

    if (num_formats == 0 || num_modifiers == 0)
    {
        goto done;
    }

    qsort(fourccs.data, num_formats, sizeof(uint32_t), eplWlCompareU32);

    // Allocate enough space for the WlFormatList itself, plus all
    // of the format structs, plus all of the modifier lists.
    data = malloc(sizeof(WlFormatList)
            + num_formats * sizeof(WlDmaBufFormat)
            + num_modifiers * sizeof(uint64_t));
    if (data == NULL)
    {
        goto done;
    }
    data->formats = (WlDmaBufFormat *) (data + 1);
    mods_dst = (uint64_t *) (data->formats + num_formats);

    data->num_formats = 0;

    wl_array_for_each(fourcc, &fourccs)
    {
        WlDmaBufFormat *fmt = &data->formats[data->num_formats++];
        const WlDmaBufFeedbackTableEntry *mod;
        fmt->fourcc = *fourcc;

        fmt->fmt = eplFormatInfoLookup(*fourcc);
        assert(fmt->fmt != NULL);

        fmt->modifiers = mods_dst;
        fmt->num_modifiers = 0;

        wl_array_for_each(mod, &modifiers)
        {
            if (mod->fourcc == *fourcc)
            {
                fmt->modifiers[fmt->num_modifiers++] = mod->modifier;
            }
        }
        mods_dst += fmt->num_modifiers;
    }

    if (ret_main_device != NULL)
    {
        *ret_main_device = state->base.main_device;
    }

done:
    wl_array_release(&fourccs);
    wl_array_release(&modifiers);
    return data;
}

static void ProcessDefaultTranche(DefaultFeedbackState *state)
{
    if (!state->base.error && state->tranche_formats.size > 0)
    {
        DefaultFeedbackTranche *tranche = calloc(1, sizeof(DefaultFeedbackTranche));
        if (tranche == NULL)
        {
            state->base.error = EGL_TRUE;
            return;
        }

        tranche->target_device = state->base.tranche_target_device;
        tranche->flags = state->base.tranche_flags;
        tranche->formats = state->tranche_formats;
        wl_array_init(&state->tranche_formats);

        glvnd_list_append(&tranche->entry, &state->tranches);
    }

    eplWlDmaBufFeedbackCommonTrancheDone(&state->base);
}

static void OnDefaultFeedbackDone(void *userdata,
        struct zwp_linux_dmabuf_feedback_v1 *wfeedback)
{
    DefaultFeedbackState *state = userdata;
    state->done = EGL_TRUE;
}

static void OnDefaultFeedbackTrancheDone(void *userdata,
        struct zwp_linux_dmabuf_feedback_v1 *wfeedback)
{
    DefaultFeedbackState *state = userdata;
    ProcessDefaultTranche(state);
}

static void OnDefaultFeedbackTrancheFormats(void *userdata,
            struct zwp_linux_dmabuf_feedback_v1 *wfeedback,
            struct wl_array *indices)
{
    DefaultFeedbackState *state = userdata;
    WlDmaBufFeedbackTableEntry *ptr;
    size_t num;
    uint16_t *index;

    if (state->base.error)
    {
        return;
    }

    if (indices->size < sizeof(uint16_t) || state->base.format_table == NULL)
    {
        return;
    }

    num = indices->size / sizeof(uint16_t);

    ptr = wl_array_add(&state->tranche_formats, num * sizeof(WlDmaBufFeedbackTableEntry));
    if (ptr == NULL)
    {
        state->base.error = EGL_TRUE;
        return;
    }

    wl_array_for_each(index, indices)
    {
        WlDmaBufFeedbackTableEntry *entry = ptr++;
        if (*index < state->base.format_table_len)
        {
            *entry = state->base.format_table[*index];
        }
        else
        {
            // Fill in DRM_FORMAT_INVALID, and we'll filter it out when we
            // compile the results at the end.
            entry->fourcc = DRM_FORMAT_INVALID;
            entry->modifier = DRM_FORMAT_MOD_INVALID;
        }
    }
}

static const struct zwp_linux_dmabuf_feedback_v1_listener DEFAULT_FEEDBACK_LISTENER =
{
    OnDefaultFeedbackDone,
    eplWlDmaBufFeedbackCommonFormatTable,
    eplWlDmaBufFeedbackCommonMainDevice,
    OnDefaultFeedbackTrancheDone,
    eplWlDmaBufFeedbackCommonTrancheTargetDevice,
    OnDefaultFeedbackTrancheFormats,
    eplWlDmaBufFeedbackCommonTrancheFlags,
};

EGLBoolean GetDefaultFeedbackV4(DefaultFeedbackState *state,
        struct wl_display *wdpy, struct zwp_linux_dmabuf_v1 *wdmabuf)
{
    struct zwp_linux_dmabuf_v1 *wrapper = NULL;
    struct zwp_linux_dmabuf_feedback_v1 *wfeedback = NULL;
    struct wl_event_queue *queue = NULL;
    EGLBoolean success = EGL_FALSE;

    glvnd_list_init(&state->tranches);

    assert(wl_proxy_get_version((struct wl_proxy *) wdmabuf)
            >= ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION);

    queue = wl_display_create_queue(wdpy);
    if (queue == NULL)
    {
        goto done;
    }

    wrapper = wl_proxy_create_wrapper(wdmabuf);
    if (wrapper == NULL)
    {
        goto done;
    }

    wl_proxy_set_queue((struct wl_proxy *) wrapper, queue);
    wfeedback = zwp_linux_dmabuf_v1_get_default_feedback(wrapper);
    if (wfeedback == NULL)
    {
        goto done;
    }

    zwp_linux_dmabuf_feedback_v1_add_listener(wfeedback, &DEFAULT_FEEDBACK_LISTENER, state);
    while (!state->done)
    {
        if (wl_display_roundtrip_queue(wdpy, queue) < 0)
        {
            state->base.error = EGL_TRUE;
            break;
        }
    }

    success = EGL_TRUE;

done:
    if (wfeedback != NULL)
    {
        zwp_linux_dmabuf_feedback_v1_destroy(wfeedback);
    }
    if (wrapper != NULL)
    {
        wl_proxy_wrapper_destroy(wrapper);
    }
    if (queue != NULL)
    {
        wl_event_queue_destroy(queue);
    }
    return success;
}

static void on_dmabuf_format(void *userdata, struct zwp_linux_dmabuf_v1 *wdmabuf, uint32_t format)
{
    // Ignore this event. We only care about formats with modifiers, so we can
    // get those from zwp_linux_dmabuf_v1:event::modifier.
}
static void on_dmabuf_modifier(void *userdata, struct zwp_linux_dmabuf_v1 *wdmabuf,
			 uint32_t format, uint32_t modifier_hi, uint32_t modifier_lo)
{
    DefaultFeedbackState *state = userdata;
    uint64_t modifier;

    if (state == NULL || state->base.error)
    {
        // We shouldn't get any events after the initial batch, but if we do,
        // ignore them.
        return;
    }

    modifier = ((uint64_t) modifier_hi) << 32 | modifier_lo;
    if (modifier != DRM_FORMAT_MOD_INVALID)
    {
        WlDmaBufFeedbackTableEntry *elem = wl_array_add(&state->tranche_formats, sizeof(WlDmaBufFeedbackTableEntry));
        if (elem == NULL)
        {
            state->base.error = EGL_TRUE;
            return;
        }

        elem->fourcc = format;
        elem->modifier = modifier;
    }
}
static const struct zwp_linux_dmabuf_v1_listener DMABUF_LISTENER =
{
    on_dmabuf_format,
    on_dmabuf_modifier,
};

static EGLBoolean GetDefaultFeedbackV3(DefaultFeedbackState *state,
        struct wl_display *wdpy, struct zwp_linux_dmabuf_v1 *wdmabuf,
        struct wl_event_queue *queue)
{
    zwp_linux_dmabuf_v1_add_listener(wdmabuf, &DMABUF_LISTENER, state);
    wl_display_roundtrip_queue(wdpy, queue);

    ProcessDefaultTranche(state);    

    // We shouldn't get any events after this, but if we do, clear the userdata
    // pointer so that we ignore them.
    wl_proxy_set_user_data((struct wl_proxy *) wdmabuf, NULL);

    return EGL_TRUE;
}

WlFormatList *eplWlDmaBufFeedbackGetDefault(struct wl_display *wdpy,
        struct zwp_linux_dmabuf_v1 *wdmabuf,
        struct wl_event_queue *queue,
        dev_t *ret_main_device)
{
    uint32_t version = wl_proxy_get_version((struct wl_proxy *) wdmabuf);
    DefaultFeedbackState state = {};
    WlFormatList *result = NULL;
    EGLBoolean success = EGL_FALSE;

    eplWlDmaBufFeedbackCommonInit(&state.base);
    glvnd_list_init(&state.tranches);
    wl_array_init(&state.tranche_formats);

    if (version >= ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION)
    {
        success = GetDefaultFeedbackV4(&state, wdpy, wdmabuf);
    }
    else if (version >= ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION)
    {
        success = GetDefaultFeedbackV3(&state, wdpy, wdmabuf, queue);
    }
    else
    {
        success = EGL_FALSE;
    }

    if (success)
    {
        result = FinishDefaultFeedback(&state, ret_main_device);
    }

    while (!glvnd_list_is_empty(&state.tranches))
    {
        DefaultFeedbackTranche *tranche = glvnd_list_first_entry(&state.tranches, DefaultFeedbackTranche, entry);
        glvnd_list_del(&tranche->entry);
        wl_array_release(&tranche->formats);
        free(tranche);
    }
    wl_array_release(&state.tranche_formats);
    return result;
}

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
        const dev_t *render_devices,
        size_t render_device_count,
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
        EGLBoolean is_render_device = EGL_FALSE;
        size_t i;

        for (i=0; i<render_device_count; i++)
        {
            if (tranche->target_device == render_devices[i])
            {
                is_render_device = EGL_TRUE;
                break;
            }
        }
        if (is_render_device)
        {
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
        const dev_t *render_devices,
        size_t render_device_count,
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
        ssize_t ret = eplWlDmaBufGetSupportedTrancheModifiers(tranche, render_devices,
                render_device_count, fourcc, driver_mods, num_driver_mods,
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
