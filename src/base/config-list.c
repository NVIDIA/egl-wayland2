/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

#include "config-list.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <EGL/egl.h>
#include <drm_fourcc.h>

#include "platform-utils.h"

const EplFormatInfo FORMAT_INFO_LIST[] =
{
    { DRM_FORMAT_ARGB8888, 32, { 8, 8, 8, 8 }, { 16, 8,  0, 24 } },
    { DRM_FORMAT_RGBA8888, 32, { 8, 8, 8, 8 }, { 24, 16, 8, 0  } },
    { DRM_FORMAT_XRGB8888, 32, { 8, 8, 8, 0 }, { 16, 8,  0, 0  } },
    { DRM_FORMAT_RGB888,   24, { 8, 8, 8, 0 }, { 16, 8,  0, 0  } },

    { DRM_FORMAT_XBGR8888, 32, { 8, 8, 8, 0 }, { 0, 8, 16, 0 } },
    { DRM_FORMAT_ABGR8888, 32, { 8, 8, 8, 8 }, { 0, 8, 16, 24 } },
    { DRM_FORMAT_R8, 8, { 8, 0, 0, 0 }, { 0, 0, 0, 0 } },
    { DRM_FORMAT_RG88, 16, { 8, 8, 0, 0 }, { 8, 0, 0, 0 } },
    { DRM_FORMAT_R16, 16, { 16, 0, 0, 0 }, { 0, 0, 0, 0 } },
    { DRM_FORMAT_RG1616, 32, { 16, 16, 0, 0 }, { 16, 0, 0, 0 } },
    { DRM_FORMAT_ARGB2101010, 32, { 10, 10, 10, 2 }, { 20, 10, 0, 30 } },
    { DRM_FORMAT_ABGR2101010, 32, { 10, 10, 10, 2 }, { 0, 10, 20, 30 } },
    { DRM_FORMAT_ABGR16161616F, 64, { 16, 16, 16, 16 }, {  0, 16, 32, 48 } },

    /* 8 bpp RGB */
    { DRM_FORMAT_RGB332, 8, { 3, 3, 2, 0 }, { 5, 2, 0, 0 } },

    /* 16 bpp RGB */
    { DRM_FORMAT_ARGB4444, 16, { 4, 4, 4, 4 }, { 8,  4, 0, 12 } },
    { DRM_FORMAT_ABGR4444, 16, { 4, 4, 4, 4 }, { 0,  4, 8, 12 } },
    { DRM_FORMAT_RGBA4444, 16, { 4, 4, 4, 4 }, { 12, 8, 4,  0 } },
    { DRM_FORMAT_BGRA4444, 16, { 4, 4, 4, 4 }, { 4,  8, 12, 0 } },
    { DRM_FORMAT_XRGB4444, 16, { 4, 4, 4, 0 }, { 8,  4, 0,  0 } },
    { DRM_FORMAT_XBGR4444, 16, { 4, 4, 4, 0 }, { 0,  4, 8,  0 } },
    { DRM_FORMAT_RGBX4444, 16, { 4, 4, 4, 0 }, { 12, 8, 4,  0 } },
    { DRM_FORMAT_BGRX4444, 16, { 4, 4, 4, 0 }, { 4,  8, 12, 0 } },

    { DRM_FORMAT_XRGB1555, 16, { 5, 5, 5, 0 }, { 10, 5, 0, 0 } },
    { DRM_FORMAT_XBGR1555, 16, { 5, 5, 5, 0 }, { 0, 5, 10, 0 } },
    { DRM_FORMAT_RGBX5551, 16, { 5, 5, 5, 0 }, { 11, 6, 1, 0 } },
    { DRM_FORMAT_BGRX5551, 16, { 5, 5, 5, 0 }, { 1, 6, 11, 0 } },

    { DRM_FORMAT_ARGB1555, 16, { 5, 5, 5, 1 }, { 10, 5, 0, 15 } },
    { DRM_FORMAT_ABGR1555, 16, { 5, 5, 5, 1 }, { 0, 5, 10, 15 } },
    { DRM_FORMAT_RGBA5551, 16, { 5, 5, 5, 1 }, { 11, 6, 1, 0 } },
    { DRM_FORMAT_BGRA5551, 16, { 5, 5, 5, 1 }, { 1, 6, 11, 0 } },

    { DRM_FORMAT_RGB565, 16, { 5, 6, 5, 0 }, { 11, 5, 0, 0 } },
    { DRM_FORMAT_BGR565, 16, { 5, 6, 5, 0 }, { 0, 5, 11, 0 } },

    /* 24 bpp RGB */
    //{ DRM_FORMAT_RGB888, 24, { 8, 8, 8, 0 }, { 16, 8, 0, 0 } },
    { DRM_FORMAT_BGR888, 24, { 8, 8, 8, 0 }, { 0, 8, 16, 0 } },

    /* 32 bpp RGB */
    { DRM_FORMAT_RGBX8888, 32, { 8, 8, 8, 0 }, { 24, 16,  8, 0 } },
    { DRM_FORMAT_BGRX8888, 32, { 8, 8, 8, 0 }, {  8, 16, 24, 0 } },
    //{ DRM_FORMAT_RGBA8888, 32, { 8, 8, 8, 8 }, { 24, 16,  8, 0 } },
    { DRM_FORMAT_BGRA8888, 32, { 8, 8, 8, 8 }, {  8, 16, 24, 0 } },
    { DRM_FORMAT_XRGB2101010, 32, { 10, 10, 10, 0 }, { 20, 10,  0, 0 } },
    { DRM_FORMAT_XBGR2101010, 32, { 10, 10, 10, 0 }, {  0, 10, 20, 0 } },
    { DRM_FORMAT_RGBX1010102, 32, { 10, 10, 10, 0 }, { 22, 12,  2, 0 } },
    { DRM_FORMAT_BGRX1010102, 32, { 10, 10, 10, 0 }, {  2, 12, 22, 0 } },
    { DRM_FORMAT_RGBA1010102, 32, { 10, 10, 10, 2 }, { 22, 12,  2, 0 } },
    { DRM_FORMAT_BGRA1010102, 32, { 10, 10, 10, 2 }, {  2, 12, 22, 0 } },

    { DRM_FORMAT_INVALID }
};
const int FORMAT_INFO_COUNT = (sizeof(FORMAT_INFO_LIST) / sizeof(FORMAT_INFO_LIST[0])) - 1;

// Note: Since the EGLConfig is the first element of EplConfig, this function
// should work for sorting and searching an array of EGLConfig or EplConfig.
static int CompareConfig(const void *p1, const void *p2)
{
    uintptr_t v1 = *((const uintptr_t *) p1);
    uintptr_t v2 = *((const uintptr_t *) p2);
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

static void LookupConfigInfo(EplPlatformData *platform, EGLDisplay edpy, EGLConfig config, EplConfig *info)
{
    EGLint color[4] = { 0, 0, 0, 0 };
    EGLint surfaceMask = 0;
    EGLint i;

    memset(info, 0, sizeof(*info));
    info->config = config;
    info->nativeVisualID = 0;
    info->nativeVisualType = EGL_NONE;

    if (!platform->egl.GetConfigAttrib(edpy, config, EGL_RED_SIZE, &color[0])
            || !platform->egl.GetConfigAttrib(edpy, config, EGL_GREEN_SIZE, &color[1])
            || !platform->egl.GetConfigAttrib(edpy, config, EGL_BLUE_SIZE, &color[2])
            || !platform->egl.GetConfigAttrib(edpy, config, EGL_ALPHA_SIZE, &color[3])
            || !platform->egl.GetConfigAttrib(edpy, config, EGL_SURFACE_TYPE, &surfaceMask))
    {
        return;
    }

    info->surfaceMask = surfaceMask;

    // For now, just find a format with the right color sizes.
    info->fourcc = DRM_FORMAT_INVALID;
    for (i=0; i<FORMAT_INFO_COUNT; i++)
    {
        if (FORMAT_INFO_LIST[i].colors[0] == color[0]
                && FORMAT_INFO_LIST[i].colors[1] == color[1]
                && FORMAT_INFO_LIST[i].colors[2] == color[2]
                && FORMAT_INFO_LIST[i].colors[3] == color[3])
        {
            info->fourcc = FORMAT_INFO_LIST[i].fourcc;
            break;
        }
    }
}

EplConfigList *eplConfigListCreate(EplPlatformData *platform, EGLDisplay edpy)
{
    EplConfigList *list = NULL;
    EGLConfig *driverConfigs = NULL;
    EGLint numConfigs = 0;
    EGLint i;

    if (!platform->egl.GetConfigs(edpy, NULL, 0, &numConfigs) || numConfigs <= 0)
    {
        return NULL;
    }

    driverConfigs = malloc(numConfigs * sizeof(EGLConfig));
    if (driverConfigs == NULL)
    {
        eplSetError(platform, EGL_BAD_ALLOC, "Out of memory");
        return NULL;
    }
    if (!platform->egl.GetConfigs(edpy, driverConfigs, numConfigs, &numConfigs) || numConfigs <= 0)
    {
        free(driverConfigs);
        return NULL;
    }
    qsort(driverConfigs, numConfigs, sizeof(EGLConfig), CompareConfig);

    list = malloc(sizeof(EplConfigList) + numConfigs * sizeof(EplConfig));
    if (list == NULL)
    {
        eplSetError(platform, EGL_BAD_ALLOC, "Out of memory");
        free(driverConfigs);
        return NULL;
    }

    list->configs = (EplConfig *) (list + 1);
    list->num_configs = numConfigs;
    for (i=0; i<numConfigs; i++)
    {
        LookupConfigInfo(platform, edpy, driverConfigs[i], &list->configs[i]);
    }
    free(driverConfigs);

    return list;
}

void eplConfigListFree(EplConfigList *list)
{
    free(list);
}

EplConfig *eplConfigListFind(EplConfigList *list, EGLConfig config)
{
    EplConfig *found = bsearch(&config, list->configs,
            list->num_configs, sizeof(EplConfig), CompareConfig);
    return found;
}

EGLint eplConfigListFindIndex(EplConfigList *list, EGLConfig config)
{
    EplConfig *found = eplConfigListFind(list, config);
    if (found != NULL)
    {
        return (EGLint) (found - list->configs);
    }
    else
    {
        return -1;
    }
}

EplConfig **eplConfigListChooseConfigs(EplPlatformData *platform, EGLDisplay edpy,
        EplConfigList *list, const EGLint *attribs,
        EGLint *ret_count, EGLint *ret_native_pixmap)
{
    EGLint surfaceMask = EGL_WINDOW_BIT;
    EGLint nativeRenderable = EGL_DONT_CARE;
    EGLint nativeVisualType = EGL_DONT_CARE;
    EGLint numAttribs = eplCountAttribs32(attribs);

    EGLint *attribsCopy = NULL;
    EGLConfig *internalConfigs = NULL;
    EGLint internalCount = 0;
    EplConfig **configs = NULL;
    EGLBoolean success = EGL_FALSE;
    EGLint matchCount = 0;
    EGLint i;

    attribsCopy = malloc((numAttribs + 3) * sizeof(EGLint));
    if (attribsCopy == NULL)
    {
        eplSetError(platform, EGL_BAD_ALLOC, "Out of memory");
        goto done;
    }

    // Copy and filter out any attributes that we need to special case.
    numAttribs = 0;
    if (attribs != NULL)
    {
        for (i=0; attribs[i] != EGL_NONE; i += 2)
        {
            if (attribs[i] == EGL_MATCH_NATIVE_PIXMAP)
            {
                if (ret_native_pixmap != NULL)
                {
                    *ret_native_pixmap = attribs[i + 1];
                }
            }
            else if (attribs[i] == EGL_SURFACE_TYPE)
            {
                surfaceMask = attribs[i + 1];
            }
            else if (attribs[i] == EGL_NATIVE_RENDERABLE)
            {
                nativeRenderable = attribs[i + 1];
            }
            else if (attribs[i] == EGL_NATIVE_VISUAL_TYPE)
            {
                nativeVisualType = attribs[i + 1];
            }
            else
            {
                attribsCopy[numAttribs++] = attribs[i];
                attribsCopy[numAttribs++] = attribs[i + 1];
            }
        }
    }
    // Get configs for all surface types. We'll filter those manually below.
    attribsCopy[numAttribs++] = EGL_SURFACE_TYPE;
    attribsCopy[numAttribs++] = EGL_DONT_CARE;
    attribsCopy[numAttribs] = EGL_NONE;

    if (!platform->egl.ChooseConfig(edpy, attribsCopy, NULL, 0, &internalCount)
            || internalCount <= 0)
    {
        goto done;
    }

    internalConfigs = malloc(internalCount * sizeof(EGLConfig));
    configs = malloc((internalCount + 1) * sizeof(EplConfig *));
    if (internalConfigs == NULL || configs == NULL)
    {
        eplSetError(platform, EGL_BAD_ALLOC, "Out of memory");
        goto done;
    }

    if (!platform->egl.ChooseConfig(edpy, attribsCopy, internalConfigs, internalCount, &internalCount)
            || internalCount <= 0)
    {
        goto done;
    }

    matchCount = 0;
    for (i=0; i<internalCount; i++)
    {
        EplConfig *info = eplConfigListFind(list, internalConfigs[i]);
        if (info == NULL)
        {
            continue;
        }

        if (surfaceMask != EGL_DONT_CARE)
        {
            if ((info->surfaceMask & surfaceMask) != surfaceMask)
            {
                continue;
            }
        }
        if (nativeRenderable != EGL_DONT_CARE)
        {
            if (info->nativeRenderable != (nativeRenderable != 0))
            {
                continue;
            }
        }
        if (nativeVisualType != EGL_DONT_CARE)
        {
            if (info->nativeVisualType != nativeVisualType)
            {
                continue;
            }
        }

        configs[matchCount++] = info;
    }
    configs[matchCount] = NULL;
    success = EGL_TRUE;

done:
    if (success)
    {
        if (ret_count != NULL)
        {
            *ret_count = matchCount;
        }
    }
    else
    {
        free(configs);
        configs = NULL;
    }
    free(attribsCopy);
    free(internalConfigs);
    return configs;
}

void eplConfigListReturnConfigs(EplConfig **configs, EGLint count,
        EGLConfig *ret_configs, EGLint max, EGLint *ret_count)
{
    EGLint num = count;
    if (ret_configs != NULL)
    {
        EGLint i;
        if (num > max)
        {
            num = max;
        }
        for (i=0; i<num; i++)
        {
            ret_configs[i] = configs[i]->config;
        }
    }
    if (ret_count != NULL)
    {
        *ret_count = num;
    }
}

EGLBoolean eplConfigListGetAttribute(EplPlatformData *platform, EGLDisplay edpy,
        EplConfigList *list, EGLConfig config, EGLint attribute, EGLint *value)
{
    const EplConfig *info;
    EGLBoolean success = EGL_TRUE;
    EGLint val = 0;

    info = eplConfigListFind(list, config);
    if (info == NULL)
    {
        eplSetError(platform, EGL_BAD_CONFIG, "Invalid EGLConfig %p", config);
        return EGL_FALSE;
    }

    if (attribute == EGL_SURFACE_TYPE)
    {
        val = info->surfaceMask;
    }
    else if (attribute == EGL_NATIVE_VISUAL_ID)
    {
        val = info->nativeVisualID;
    }
    else if (attribute == EGL_NATIVE_VISUAL_TYPE)
    {
        val = info->nativeVisualType;
    }
    else if (attribute == EGL_NATIVE_RENDERABLE)
    {
        val = info->nativeRenderable;
    }
    else
    {
        success = platform->egl.GetConfigAttrib(edpy, config, attribute, &val);
    }
    if (success && value != NULL)
    {
        *value = val;
    }

    return success;
}

const EplFormatInfo *eplFormatInfoLookup(uint32_t fourcc)
{
    int i;
    for (i=0; i<FORMAT_INFO_COUNT; i++)
    {
        if (FORMAT_INFO_LIST[i].fourcc == fourcc)
        {
            return &FORMAT_INFO_LIST[i];
        }
    }

    return NULL;
}

