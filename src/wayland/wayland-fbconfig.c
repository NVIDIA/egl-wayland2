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

#include "wayland-fbconfig.h"
#include "wayland-display.h"

WlFormatList *eplWlGetDriverFormats(EplPlatformData *plat, EGLDisplay internal_display)
{
    EGLint *fourccs = NULL;
    EGLint count = 0;
    EGLint num_formats = 0;
    EGLint max_modifiers = 0;
    size_t total_modifiers = 0;

    WlFormatList *result = NULL;
    uint64_t *mods_base = NULL;
    size_t mods_offset = 0;
    uint64_t *modsbuf = NULL;
    EGLBoolean *extern_only = NULL;

    EGLBoolean success = EGL_FALSE;
    EGLint i;

    if (!plat->priv->egl.QueryDmaBufFormatsEXT(internal_display,
                0, NULL, &num_formats) || num_formats <= 0)
    {
        return NULL;
    }

    fourccs = malloc(num_formats * sizeof(EGLint));
    if (fourccs == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Out of memory");
        return NULL;
    }

    if (!plat->priv->egl.QueryDmaBufFormatsEXT(internal_display,
                num_formats, fourccs, &num_formats) || num_formats <= 0)
    {
        goto done;
    }

    count = 0;
    for (i=0; i<num_formats; i++)
    {
        EGLint num = 0;

        // Filter out any fourcc codes that we don't recognize.
        if (eplFormatInfoLookup(fourccs[i]) == NULL)
        {
            continue;
        }

        if (!plat->priv->egl.QueryDmaBufModifiersEXT(internal_display, fourccs[i], 0, NULL, NULL, &num))
        {
            goto done;
        }
        if (num <= 0)
        {
            continue;
        }

        fourccs[count++] = fourccs[i];
        if (num > max_modifiers)
        {
            max_modifiers = num;
        }
        total_modifiers += num;
    }
    num_formats = count;

    if (total_modifiers == 0)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Driver error: No supported format modifiers");
        goto done;
    }

    qsort(fourccs, num_formats, sizeof(EGLint), eplWlCompareU32);

    result = malloc(sizeof(WlFormatList)
            + num_formats * sizeof(WlDmaBufFormat)
            + total_modifiers * sizeof(uint64_t));
    if (result == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Out of memory");
        goto done;
    }
    result->num_formats = 0;
    result->formats = (WlDmaBufFormat *) (result + 1);
    mods_base = (uint64_t *) (result->formats + num_formats);

    modsbuf = malloc(max_modifiers * (sizeof(uint64_t) + sizeof(EGLBoolean)));
    if (modsbuf == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Out of memory");
        goto done;
    }
    extern_only = (EGLBoolean *) (modsbuf + max_modifiers);

    for (i=0; i<num_formats; i++)
    {
        WlDmaBufFormat *fmt = &result->formats[result->num_formats];
        EGLint num = 0;
        EGLint j = 0;

        if (!plat->priv->egl.QueryDmaBufModifiersEXT(internal_display, fourccs[i],
                    max_modifiers, modsbuf, extern_only, &num) || num <= 0)
        {
            goto done;
        }

        if (mods_offset + num > total_modifiers)
        {
            // This shouldn't never happen -- we should get the same number of
            // modifiers here as we did above.
            eplSetError(plat, EGL_BAD_ALLOC, "Internal error: Mismatched modifier count");
            goto done;
        }

        // Pull out the non-external modifiers
        fmt->modifiers = mods_base + mods_offset;
        fmt->num_modifiers = 0;
        for (j=0; j<num; j++)
        {
            if (!extern_only[j])
            {
                fmt->modifiers[fmt->num_modifiers++] = modsbuf[j];
            }
        }
        if (fmt->num_modifiers == 0)
        {
            // No non-external modifiers for this format. This shouldn't happen
            // in practice, but just ignore the format if it does.
            continue;
        }
        mods_offset += fmt->num_modifiers;

        fmt->fourcc = fourccs[i];
        fmt->fmt = eplFormatInfoLookup(fourccs[i]);
        result->num_formats++;
    }

    if (result->num_formats > 0)
    {
        success = EGL_TRUE;
    }

done:
    free(fourccs);
    free(modsbuf);
    if (!success)
    {
        free(result);
        result = NULL;
    }
    return result;
}

static EGLBoolean SetupConfig(EplPlatformData *plat,
        EGLDisplay internal_display,
        const WlFormatList *server_formats,
        const WlFormatList *driver_formats,
        EGLBoolean allow_prime,
        EGLBoolean force_prime,
        EplConfig *config)
{
    const WlDmaBufFormat *driver_fmt = NULL;
    const WlDmaBufFormat *server_fmt = NULL;
    EGLint fourcc = DRM_FORMAT_INVALID;
    EGLBoolean supported = EGL_FALSE;
    size_t i, j;

    config->surfaceMask &= ~(EGL_WINDOW_BIT | EGL_PIXMAP_BIT);

    // Query the fourcc code from the driver.
    if (plat->priv->egl.PlatformGetConfigAttribNVX(internal_display,
                config->config, EGL_LINUX_DRM_FOURCC_EXT, &fourcc))
    {
        config->fourcc = (uint32_t) fourcc;
    }
    else
    {
        config->fourcc = DRM_FORMAT_INVALID;
    }

    if (!EGL_PLATFORM_SURFACE_INTERFACE_CHECK_VERSION(plat->priv->egl.platform_surface_version,
                EGL_PLATFORM_SURFACE_INTERNAL_SWAP_SINCE))
    {
        // Multisampled surfaces require additional driver support which was
        // added in interface version 0.2.
        EGLint msaa = 0;
        if (plat->priv->egl.PlatformGetConfigAttribNVX(internal_display,
                    config->config, EGL_SAMPLE_BUFFERS, &msaa))
        {
            if (msaa != 0)
            {
                return EGL_TRUE;
            }
        }
    }

    if (config->fourcc == DRM_FORMAT_INVALID)
    {
        // Without a format, we can't do anything with this config.
        return EGL_TRUE;
    }

    if ((config->surfaceMask & EGL_STREAM_BIT_KHR) == 0)
    {
        return EGL_TRUE;
    }

    driver_fmt = eplWlDmaBufFormatFind(driver_formats->formats, driver_formats->num_formats, fourcc);
    if (driver_fmt == NULL)
    {
        // The driver doesn't support importing a dma-buf with this format.
        return EGL_TRUE;
    }

    server_fmt = eplWlDmaBufFormatFind(server_formats->formats, server_formats->num_formats, fourcc);
    if (server_fmt == NULL)
    {
        // The server doesn't support importing a dma-buf with this format.
        return EGL_TRUE;
    }

    for (i=0; i<server_fmt->num_modifiers && !supported; i++)
    {
        if (allow_prime && server_fmt->modifiers[i] == DRM_FORMAT_MOD_LINEAR)
        {
            // If the server supports linear, then we can always work with that.
            supported = EGL_TRUE;
        }
        else if (!force_prime)
        {
            for (j=0; j<driver_fmt->num_modifiers && !supported; j++)
            {
                if (server_fmt->modifiers[i] == driver_fmt->modifiers[j])
                {
                    supported = EGL_TRUE;
                }
            }
        }
    }

    if (supported)
    {
        config->surfaceMask |= EGL_WINDOW_BIT;
    }
    return EGL_TRUE;
}

EplConfigList *eplWlInitConfigList(EplPlatformData *plat,
        EGLDisplay internal_display,
        const WlFormatList *server_formats,
        const WlFormatList *driver_formats,
        EGLBoolean allow_prime,
        EGLBoolean force_prime,
        EGLBoolean from_init)
{
    int i;
    EplConfigList *configs = NULL;
    EGLBoolean any_supported = EGL_FALSE;

    configs = eplConfigListCreate(plat, internal_display);
    if (configs == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Can't find any usable EGLConfigs");
        return NULL;
    }

    for (i=0; i<configs->num_configs; i++)
    {
        if (!SetupConfig(plat, internal_display, server_formats, driver_formats,
                    allow_prime, force_prime, &configs->configs[i]))
        {
            eplConfigListFree(configs);
            return NULL;
        }
        if (configs->configs[i].surfaceMask & EGL_WINDOW_BIT)
        {
            any_supported = EGL_TRUE;
        }
    }

    if (!any_supported)
    {
        eplConfigListFree(configs);
        if (from_init)
        {
            eplSetError(plat, EGL_BAD_ALLOC, "Can't find any supported EGLConfigs");
        }
        return NULL;
    }

    return configs;
}

EGLBoolean eplWlHookChooseConfig(EGLDisplay edpy, EGLint const *attribs,
        EGLConfig *configs, EGLint configSize, EGLint *numConfig)
{
    EplDisplay *pdpy;
    EGLint matchNativePixmap = EGL_DONT_CARE;
    EGLBoolean success = EGL_FALSE;
    EplConfig **found = NULL;
    EGLint count = 0;

    pdpy = eplDisplayAcquire(edpy);
    if (pdpy == NULL)
    {
        return EGL_FALSE;
    }

    found = eplConfigListChooseConfigs(pdpy->platform, pdpy->internal_display,
            pdpy->priv->inst->configs, attribs, &count, &matchNativePixmap);
    if (found == NULL)
    {
        goto done;
    }

    if (matchNativePixmap != EGL_DONT_CARE)
    {
        // Wayland doesn't have pixmaps, so no EGLConfig can match one.
        count = 0;
    }

    success = EGL_TRUE;

done:
    if (success)
    {
        eplConfigListReturnConfigs(found, count, configs, configSize, numConfig);
    }
    free(found);
    eplDisplayRelease(pdpy);
    return success;
}

EGLBoolean eplWlHookGetConfigAttrib(EGLDisplay edpy, EGLConfig config,
        EGLint attribute, EGLint *value)
{
    EplDisplay *pdpy = eplDisplayAcquire(edpy);
    EGLBoolean success = EGL_TRUE;

    if (pdpy == NULL)
    {
        return EGL_FALSE;
    }

    success = eplConfigListGetAttribute(pdpy->platform, pdpy->internal_display,
            pdpy->priv->inst->configs, config, attribute, value);

    eplDisplayRelease(pdpy);

    return success;
}
