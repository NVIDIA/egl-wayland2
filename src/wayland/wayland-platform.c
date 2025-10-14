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
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <assert.h>

#include "wayland-display.h"
#include "wayland-fbconfig.h"
#include "platform-utils.h"
#include "dma-buf.h"

static const EGLint NEED_PLATFORM_SURFACE_MAJOR = 0;
static const EGLint NEED_PLATFORM_SURFACE_MINOR = 1;

static void eplWlCleanupPlatform(EplPlatformData *plat);
static const char *eplWlQueryString(EplPlatformData *plat, EplDisplay *pdpy, EGLExtPlatformString name);
static void *eplWlGetHookFunction(EplPlatformData *plat, const char *name);

/**
 * True if the kernel might support DMA_BUF_IOCTL_IMPORT_SYNC_FILE and
 * DMA_BUF_IOCTL_EXPORT_SYNC_FILE.
 *
 * There's no direct way to query that support, so instead, if an ioctl fails,
 * then we set this flag to false so that we don't waste time trying again.
 */
static EGLBoolean import_sync_file_supported = EGL_TRUE;
static pthread_mutex_t import_sync_file_supported_mutex = PTHREAD_MUTEX_INITIALIZER;

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
    .CreateWindowSurface = eplWlCreateWindowSurface,
    .DestroySurface = eplWlDestroyWindow,
    .SwapBuffers = eplWlSwapBuffers,
    .WaitGL = eplWlWaitGL,
    .SwapInterval = eplWlSwapInterval,
    .QueryBufferAge = eplWlQueryBufferAge,
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

static struct gbm_bo *fallback_gbo_create_with_modifiers2(struct gbm_device *gbm,
        uint32_t width, uint32_t height,
        uint32_t format,
        const uint64_t *modifiers,
        const unsigned int count,
        uint32_t flags)
{
    return gbm_bo_create_with_modifiers(gbm, width, height, format, modifiers, count);
}

PUBLIC EGLBoolean loadEGLExternalPlatform(int major, int minor,
                                   const EGLExtDriver *driver,
                                   EGLExtPlatform *extplatform)
{
    EplPlatformData *plat = NULL;
    EGLBoolean timelineSupported = EGL_TRUE;
    pfn_eglPlatformGetVersionNVX ptr_eglPlatformGetVersionNVX;
    void *dlHandle = RTLD_DEFAULT;

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

    // wl_display_create_queue_with_name was added in libwayland 1.22.91. Use
    // it if it's available, but we don't otherwise need anything that recent.
    plat->priv->wl.display_create_queue_with_name = dlsym(RTLD_DEFAULT,
                                                          "wl_display_create_queue_with_name");

    // try to find drmGetDeviceFromDevId. First try the default search method,
    // but certain application tricks may interfere with this. Most notably
    // steam's overlay. If we can't find it through default methods fall back
    // to directly opening libdrm.
    plat->priv->drm.GetDeviceFromDevId = dlsym(RTLD_DEFAULT, "drmGetDeviceFromDevId");
    if (!plat->priv->drm.GetDeviceFromDevId)
    {
        plat->priv->drm.libdrmDlHandle = dlopen("libdrm.so.2", RTLD_LAZY);
        if (plat->priv->drm.libdrmDlHandle)
        {
            plat->priv->drm.GetDeviceFromDevId = dlsym(plat->priv->drm.libdrmDlHandle,
                                                       "drmGetDeviceFromDevId");
            dlHandle = plat->priv->drm.libdrmDlHandle;
        }
    }

#define LOAD_PROC(supported, prefix, group, name) \
    supported = supported && LoadProcHelper(plat, dlHandle, (void **) &plat->priv->group.name, prefix #name)

    // Load the functions that we'll need for explicit sync, if they're
    // available. If we don't find these, then it's not fatal.
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

    // Load gbm_bo_create_with_modifiers2 if it's available. If it's not, then
    // we'll fall back to using gbm_bo_create_with_modifiers.
    plat->priv->gbm.bo_create_with_modifiers2 = dlsym(RTLD_DEFAULT, "gbm_bo_create_with_modifiers2");
    if (plat->priv->gbm.bo_create_with_modifiers2 == NULL)
    {
        plat->priv->gbm.bo_create_with_modifiers2 = fallback_gbo_create_with_modifiers2;
    }

    eplPlatformBaseInitFinish(plat);
    return EGL_TRUE;
}

void eplWlCleanupPlatform(EplPlatformData *plat)
{
    if (plat->priv->drm.libdrmDlHandle)
    {
        dlclose(plat->priv->drm.libdrmDlHandle);
    }
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
    if (strcmp(name, "eglChooseConfig") == 0)
    {
        return eplWlHookChooseConfig;
    }
    else if (strcmp(name, "eglGetConfigAttrib") == 0)
    {
        return eplWlHookGetConfigAttrib;
    }
    else if (strcmp(name, "eglQueryString") == 0)
    {
        return eplWlHookQueryString;
    }
    return NULL;
}

EGLDeviceEXT eplWlFindDeviceForNode(EplPlatformData *plat, const char *node)
{
    EGLDeviceEXT *devices = NULL;
    EGLDeviceEXT found = EGL_NO_DEVICE_EXT;
    EGLint num = 0;
    int i;

    if (!plat->egl.QueryDevicesEXT(0, NULL, &num) || num <= 0)
    {
        return EGL_NO_DEVICE_EXT;
    }

    devices = alloca(num * sizeof(EGLDeviceEXT));
    if (!plat->egl.QueryDevicesEXT(num, devices, &num) || num <= 0)
    {
        return EGL_NO_DEVICE_EXT;
    }

    for (i=0; i<num; i++)
    {
        const char *extensions = plat->egl.QueryDeviceStringEXT(devices[i], EGL_EXTENSIONS);
        if (eplFindExtension("EGL_EXT_device_drm", extensions))
        {
            const char *str = plat->egl.QueryDeviceStringEXT(devices[i], EGL_DRM_DEVICE_FILE_EXT);
            if (str != NULL && strcmp(str, node) == 0)
            {
                found = devices[i];
                break;
            }
        }
        if (eplFindExtension("EGL_EXT_device_drm_render_node", extensions))
        {
            const char *str = plat->egl.QueryDeviceStringEXT(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);
            if (str != NULL && strcmp(str, node) == 0)
            {
                found = devices[i];
                break;
            }
        }
    }

    return found;
}

/**
 * Returns true if the kernel might support DMA_BUF_IOCTL_IMPORT_SYNC_FILE and
 * DMA_BUF_IOCTL_EXPORT_SYNC_FILE.
 *
 * Note that we don't actually know whether it does until we try to use them,
 * so this really just provides an early-out to some cases.
 */
static EGLBoolean eplWlCheckImportSyncFileSupported(void)
{
    EGLBoolean ret;
    pthread_mutex_lock(&import_sync_file_supported_mutex);
    ret = import_sync_file_supported;
    pthread_mutex_unlock(&import_sync_file_supported_mutex);
    return ret;
}

static void eplWlSetImportSyncFileUnsupported(void)
{
    pthread_mutex_lock(&import_sync_file_supported_mutex);
    import_sync_file_supported = EGL_FALSE;
    pthread_mutex_unlock(&import_sync_file_supported_mutex);
}

EGLBoolean eplWlImportDmaBufSyncFile(int dmabuf, int syncfd)
{
    EGLBoolean ret = EGL_FALSE;

    if (eplWlCheckImportSyncFileSupported())
    {
        struct dma_buf_import_sync_file params = {};

        params.flags = DMA_BUF_SYNC_WRITE;
        params.fd = syncfd;
        if (drmIoctl(dmabuf, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &params) == 0)
        {
            ret = EGL_TRUE;
        }
        else if (errno == ENOTTY || errno == EBADF || errno == ENOSYS)
        {
            eplWlSetImportSyncFileUnsupported();
        }
    }

    return ret;
}

int eplWlExportDmaBufSyncFile(int dmabuf)
{
    int fd = -1;

    if (eplWlCheckImportSyncFileSupported())
    {
        struct dma_buf_export_sync_file params = {};
        params.flags = DMA_BUF_SYNC_WRITE;
        params.fd = -1;

        if (drmIoctl(dmabuf, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &params) == 0)
        {
            fd = params.fd;
        }
        else if (errno == ENOTTY || errno == EBADF || errno == ENOSYS)
        {
            eplWlSetImportSyncFileUnsupported();
        }
    }

    return fd;
}
