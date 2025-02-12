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

#include "wayland-platform.h"

#include <stdlib.h>
#include <dlfcn.h>
#include <assert.h>

static const EGLint NEED_PLATFORM_SURFACE_MAJOR = 0;
static const EGLint NEED_PLATFORM_SURFACE_MINOR = 1;

static void eplWlCleanupPlatform(EplPlatformData *plat);
static const char *eplWlQueryString(EplPlatformData *plat, EplDisplay *pdpy, EGLExtPlatformString name);
static void *eplWlGetHookFunction(EplPlatformData *plat, const char *name);
static EGLBoolean eplWlIsSameDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint platform,
        void *native_display, const EGLAttrib *attribs);
static EGLBoolean eplWlGetPlatformDisplay(EplPlatformData *plat, EplDisplay *pdpy,
        void *native_display, const EGLAttrib *attribs,
        struct glvnd_list *existing_displays);
static void eplWlCleanupDisplay(EplDisplay *pdpy);
static EGLBoolean eplWlInitializeDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint *major, EGLint *minor);
static void eplWlTerminateDisplay(EplPlatformData *plat, EplDisplay *pdpy);

static void eplWlDestroyWindow(EplDisplay *pdpy, EplSurface *psurf,
        const struct glvnd_list *existing_surfaces) { }

static const EplImplFuncs WL_IMPL_FUNCS =
{
    .CleanupPlatform = eplWlCleanupPlatform,
    .QueryString = eplWlQueryString,
    .GetHookFunction = eplWlGetHookFunction,
    .IsSameDisplay = eplWlIsSameDisplay,
    .GetPlatformDisplay = eplWlGetPlatformDisplay,
    .CleanupDisplay = eplWlCleanupDisplay,
    .InitializeDisplay = eplWlInitializeDisplay,
    .TerminateDisplay = eplWlTerminateDisplay,
    .CreateWindowSurface = NULL,
    .DestroySurface = eplWlDestroyWindow,
    .SwapBuffers = NULL,
};

static EGLBoolean LoadProcHelper(EplPlatformData *plat, void *handle, void **ptr, const char *name)
{
    *ptr = dlsym(handle, name);
    if (*ptr == NULL)
    {
        return EGL_FALSE;
    }
    return EGL_TRUE;
}

PUBLIC EGLBoolean loadEGLExternalPlatform(int major, int minor,
                                   const EGLExtDriver *driver,
                                   EGLExtPlatform *extplatform)
{
    EplPlatformData *plat = NULL;
    EGLBoolean timelineSupported = EGL_TRUE;
    pfn_eglPlatformGetVersionNVX ptr_eglPlatformGetVersionNVX;

    // Before we do anything else, make sure that we've got a recent enough
    // version of libgbm.
    // Note that we don't yet reference anything in libgbm, so we won't bring in
    // libgbm yet.
    /*
    if (dlsym(RTLD_DEFAULT, "gbm_bo_create_with_modifiers2") == NULL)
    {
        return EGL_FALSE;
    }
    */

    plat = eplPlatformBaseAllocate(major, minor,
        driver, extplatform, EGL_PLATFORM_WAYLAND_KHR, &WL_IMPL_FUNCS,
        sizeof(EplImplPlatform));
    if (plat == NULL)
    {
        return EGL_FALSE;
    }

    ptr_eglPlatformGetVersionNVX = driver->getProcAddress("eglPlatformGetVersionNVX");
    if (ptr_eglPlatformGetVersionNVX == NULL
            || !EGL_PLATFORM_SURFACE_INTERFACE_CHECK_VERSION(ptr_eglPlatformGetVersionNVX(),
                NEED_PLATFORM_SURFACE_MAJOR, NEED_PLATFORM_SURFACE_MINOR))
    {
        // The driver doesn't support a compatible version of the platform
        // surface interface.
        eplPlatformBaseInitFail(plat);
        return EGL_FALSE;
    }

    plat->priv->egl.QueryDisplayAttribKHR = driver->getProcAddress("eglQueryDisplayAttribKHR");
    plat->priv->egl.SwapInterval = driver->getProcAddress("eglSwapInterval");
    plat->priv->egl.QueryDmaBufFormatsEXT = driver->getProcAddress("eglQueryDmaBufFormatsEXT");
    plat->priv->egl.QueryDmaBufModifiersEXT = driver->getProcAddress("eglQueryDmaBufModifiersEXT");
    plat->priv->egl.CreateSync = driver->getProcAddress("eglCreateSync");
    plat->priv->egl.DestroySync = driver->getProcAddress("eglDestroySync");
    plat->priv->egl.WaitSync = driver->getProcAddress("eglWaitSync");
    plat->priv->egl.DupNativeFenceFDANDROID = driver->getProcAddress("eglDupNativeFenceFDANDROID");
    plat->priv->egl.Flush = driver->getProcAddress("glFlush");
    plat->priv->egl.Finish = driver->getProcAddress("glFinish");
    plat->priv->egl.PlatformImportColorBufferNVX = driver->getProcAddress("eglPlatformImportColorBufferNVX");
    plat->priv->egl.PlatformFreeColorBufferNVX = driver->getProcAddress("eglPlatformFreeColorBufferNVX");
    plat->priv->egl.PlatformCreateSurfaceNVX = driver->getProcAddress("eglPlatformCreateSurfaceNVX");
    plat->priv->egl.PlatformSetColorBuffersNVX = driver->getProcAddress("eglPlatformSetColorBuffersNVX");
    plat->priv->egl.PlatformGetConfigAttribNVX = driver->getProcAddress("eglPlatformGetConfigAttribNVX");
    plat->priv->egl.PlatformCopyColorBufferNVX = driver->getProcAddress("eglPlatformCopyColorBufferNVX");
    plat->priv->egl.PlatformAllocColorBufferNVX = driver->getProcAddress("eglPlatformAllocColorBufferNVX");
    plat->priv->egl.PlatformExportColorBufferNVX = driver->getProcAddress("eglPlatformExportColorBufferNVX");

    if (plat->priv->egl.QueryDisplayAttribKHR == NULL
            || plat->priv->egl.SwapInterval == NULL
            || plat->priv->egl.QueryDmaBufFormatsEXT == NULL
            || plat->priv->egl.QueryDmaBufModifiersEXT == NULL
            || plat->priv->egl.CreateSync == NULL
            || plat->priv->egl.DestroySync == NULL
            || plat->priv->egl.WaitSync == NULL
            || plat->priv->egl.DupNativeFenceFDANDROID == NULL
            || plat->priv->egl.Finish == NULL
            || plat->priv->egl.Flush == NULL
            || plat->priv->egl.PlatformImportColorBufferNVX == NULL
            || plat->priv->egl.PlatformFreeColorBufferNVX == NULL
            || plat->priv->egl.PlatformCreateSurfaceNVX == NULL
            || plat->priv->egl.PlatformSetColorBuffersNVX == NULL
            || plat->priv->egl.PlatformGetConfigAttribNVX == NULL
            || plat->priv->egl.PlatformCopyColorBufferNVX == NULL
            || plat->priv->egl.PlatformAllocColorBufferNVX == NULL
            || plat->priv->egl.PlatformExportColorBufferNVX == NULL)
    {
        eplPlatformBaseInitFail(plat);
        return EGL_FALSE;
    }

    plat->priv->drm.GetDeviceFromDevId = dlsym(RTLD_DEFAULT, "drmGetDeviceFromDevId");

#define LOAD_PROC(supported, prefix, group, name) \
    supported = supported && LoadProcHelper(plat, RTLD_DEFAULT, (void **) &plat->priv->group.name, prefix #name)

    // Load the functions that we'll need for explicit sync, if they're
    // available. If we don't find these, then it's not fatal.
    LOAD_PROC(timelineSupported, "drm", drm, GetDeviceFromDevId);
    LOAD_PROC(timelineSupported, "drm", drm, GetCap);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjCreate);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjDestroy);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjHandleToFD);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjFDToHandle);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjImportSyncFile);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjExportSyncFile);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjTimelineSignal);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjTimelineWait);
    LOAD_PROC(timelineSupported, "drm", drm, SyncobjTransfer);

    plat->priv->timeline_funcs_supported = timelineSupported;

#undef LOAD_PROC

    eplPlatformBaseInitFinish(plat);
    return EGL_TRUE;
}

void eplWlCleanupPlatform(EplPlatformData *plat)
{
    // Nothing to do here.
}

const char *eplWlQueryString(EplPlatformData *plat, EplDisplay *pdpy, EGLExtPlatformString name)
{
    assert(plat != NULL);

    switch (name)
    {
        case EGL_EXT_PLATFORM_PLATFORM_CLIENT_EXTENSIONS:
            return "EGL_KHR_platform_wayland EGL_EXT_platform_wayland";
        case EGL_EXT_PLATFORM_DISPLAY_EXTENSIONS:
            return "";
        default:
            return NULL;
    }
}

void *eplWlGetHookFunction(EplPlatformData *plat, const char *name)
{
    // Nothing here yet.
    return NULL;
}

EGLBoolean eplWlIsSameDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint platform,
        void *native_display, const EGLAttrib *attribs)
{
    return EGL_FALSE;
}

EGLBoolean eplWlGetPlatformDisplay(EplPlatformData *plat, EplDisplay *pdpy,
        void *native_display, const EGLAttrib *attribs,
        struct glvnd_list *existing_displays)
{
    return EGL_FALSE;
}

void eplWlCleanupDisplay(EplDisplay *pdpy)
{
}

EGLBoolean eplWlInitializeDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint *major, EGLint *minor)
{
    return EGL_FALSE;
}

void eplWlTerminateDisplay(EplPlatformData *plat, EplDisplay *pdpy)
{
}

