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

#include "wayland-display.h"

#include <stdlib.h>
#include <assert.h>

static WlDisplayInstance *eplWlDisplayInstanceCreate(EplDisplay *pdpy, EGLBoolean from_init);
static void eplWlDisplayInstanceFree(WlDisplayInstance *inst);

EPL_REFCOUNT_DEFINE_TYPE_FUNCS(WlDisplayInstance, eplWlDisplayInstance, refcount, eplWlDisplayInstanceFree);

EGLBoolean eplWlIsSameDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint platform,
        void *native_display, const EGLAttrib *attribs)
{
    EGLDeviceEXT device = EGL_NO_DEVICE_EXT;
    if (attribs != NULL)
    {
        int i;
        for (i=0; attribs[i] != EGL_NONE; i += 2)
        {
            if (attribs[i] == EGL_DEVICE_EXT)
            {
                device = (EGLDeviceEXT) attribs[i + 1];
            }
            else
            {
                return EGL_FALSE;
            }
        }
    }

    if (device != pdpy->priv->device_attrib)
    {
        return EGL_FALSE;
    }

    return EGL_TRUE;
}

EGLBoolean eplWlGetPlatformDisplay(EplPlatformData *plat, EplDisplay *pdpy,
        void *native_display, const EGLAttrib *attribs,
        struct glvnd_list *existing_displays)
{
    const char *env;
    WlDisplayInstance *inst;

    pdpy->priv = calloc(1, sizeof(EplImplDisplay));
    if (pdpy->priv == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Out of memory");
        return EGL_FALSE;
    }

    if (attribs != NULL)
    {
        int i;
        for (i=0; attribs[i] != EGL_NONE; i += 2)
        {
            if (attribs[i] == EGL_DEVICE_EXT)
            {
                pdpy->priv->device_attrib = (EGLDeviceEXT) attribs[i + 1];
            }
            else
            {
                eplSetError(plat, EGL_BAD_ATTRIBUTE, "Invalid attribute 0x%lx", (unsigned long) attribs[i]);
                eplWlCleanupDisplay(pdpy);
                return EGL_FALSE;
            }
        }
    }

    env = getenv("__NV_PRIME_RENDER_OFFLOAD_PROVIDER");
    if (env != NULL)
    {
        pdpy->priv->requested_device = eplWlFindDeviceForNode(plat, env);
        pdpy->priv->enable_alt_device = EGL_TRUE;
    }
    else
    {
        env = getenv("__NV_PRIME_RENDER_OFFLOAD");
        if (env != NULL && atoi(env) != 0)
        {
            pdpy->priv->enable_alt_device = EGL_TRUE;
        }
    }

    if (pdpy->priv->requested_device == EGL_NO_DEVICE_EXT)
    {
        // If the caller specified a device, then make sure it's valid.
        if (pdpy->priv->device_attrib != EGL_NO_DEVICE_EXT)
        {
            EGLint num = 0;
            EGLDeviceEXT *devices = eplGetAllDevices(plat, &num);
            EGLBoolean valid = EGL_FALSE;
            EGLint i;

            if (devices == NULL)
            {
                eplWlCleanupDisplay(pdpy);
                return EGL_FALSE;
            }

            for (i=0; i<num; i++)
            {
                if (devices[i] == pdpy->priv->device_attrib)
                {
                    valid = EGL_TRUE;
                    break;
                }
            }
            free(devices);

            if (valid)
            {
                // The requested device is a valid NVIDIA device, so use it.
                pdpy->priv->requested_device = pdpy->priv->device_attrib;
            }
            else if (pdpy->priv->enable_alt_device)
            {
                // The requested device is not an NVIDIA device, but PRIME is
                // enabled, so we'll pick an NVIDIA device during eglInitialize.
                pdpy->priv->requested_device = EGL_NO_DEVICE_EXT;
            }
            else
            {
                // The requested device is not an NVIDIA device and PRIME is not
                // enabled. Return failure to let another driver handle it.
                eplSetError(plat, EGL_BAD_MATCH, "Unknown or non-NV device handle %p",
                        pdpy->priv->device_attrib);
                eplWlCleanupDisplay(pdpy);
                return EGL_FALSE;
            }
        }
    }

    /*
     * Ideally, we'd wait until eglInitialize to open the connection or do the
     * rest of our compatibility checks, but we have to do that now to check
     * whether we can actually support whichever server we're connecting to.
     */
    inst = eplWlDisplayInstanceCreate(pdpy, EGL_FALSE);
    if (inst == NULL)
    {
        eplWlCleanupDisplay(pdpy);
        return EGL_FALSE;
    }
    eplWlDisplayInstanceUnref(inst);

    return EGL_TRUE;
}

void eplWlCleanupDisplay(EplDisplay *pdpy)
{
    if (pdpy->priv != NULL)
    {
        eplWlDisplayInstanceUnref(pdpy->priv->inst);
        free(pdpy->priv);
        pdpy->priv = NULL;
    }
}

EGLBoolean eplWlInitializeDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint *major, EGLint *minor)
{
    assert(pdpy->priv->inst == NULL);

    pdpy->priv->inst = eplWlDisplayInstanceCreate(pdpy, EGL_TRUE);
    if (pdpy->priv->inst == NULL)
    {
        return EGL_FALSE;
    }

    if (major != NULL)
    {
        *major = pdpy->priv->inst->internal_display->major;
    }
    if (minor != NULL)
    {
        *minor = pdpy->priv->inst->internal_display->minor;
    }

    pdpy->internal_display = pdpy->priv->inst->internal_display->edpy;
    return EGL_TRUE;
}

void eplWlTerminateDisplay(EplPlatformData *plat, EplDisplay *pdpy)
{
    assert(pdpy->priv->inst != NULL);
    eplWlDisplayInstanceUnref(pdpy->priv->inst);
    pdpy->priv->inst = NULL;
}

WlDisplayInstance *eplWlDisplayInstanceCreate(EplDisplay *pdpy, EGLBoolean from_init)
{
    WlDisplayInstance *inst = NULL;
    EGLBoolean success = EGL_FALSE;

    inst = calloc(1, sizeof(WlDisplayInstance));
    if (inst == NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Out of memory");
        return NULL;
    }
    eplRefCountInit(&inst->refcount);
    inst->platform = eplPlatformDataRef(pdpy->platform);

    if (pdpy->native_display == NULL)
    {
        inst->own_display = EGL_TRUE;
        inst->wdpy = wl_display_connect(NULL);
        if (inst->wdpy == NULL)
        {
            eplSetError(pdpy->platform, EGL_BAD_ALLOC, "wl_display_connect failed");
            goto done;
        }
    }
    else
    {
        inst->own_display = EGL_FALSE;
        inst->wdpy = pdpy->native_display;
    }

    // Pick an arbitrary device to use as a placeholder for an internal EGLDisplay.
    inst->internal_display = eplInternalDisplayRef(eplGetDeviceInternalDisplay(pdpy->platform, EGL_NO_DEVICE_EXT));
    if (inst->internal_display == NULL)
    {
        goto done;
    }

    if (!eplInitializeInternalDisplay(pdpy->platform, inst->internal_display, NULL, NULL))
    {
        goto done;
    }

    success = EGL_TRUE;

done:
    if (!success)
    {
        eplWlDisplayInstanceUnref(inst);
        inst = NULL;
    }

    return inst;
}

static void eplWlDisplayInstanceFree(WlDisplayInstance *inst)
{
    if (inst != NULL)
    {
        if (inst->internal_display != NULL)
        {
            eplTerminateInternalDisplay(inst->platform, inst->internal_display);
            eplInternalDisplayUnref(inst->internal_display);
        }

        if (inst->own_display && inst->wdpy != NULL)
        {
            wl_display_disconnect(inst->wdpy);
        }

        if (inst->platform != NULL)
        {
            eplPlatformDataUnref(inst->platform);
        }

        free(inst);
    }
}

