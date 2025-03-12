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

#ifndef WAYLAND_PLATFORM_H
#define WAYLAND_PLATFORM_H

#include <stdint.h>
#include <sys/types.h>

#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <xf86drm.h>

#include "platform-base.h"
#include "platform-impl.h"
#include "driver-platform-surface.h"

struct _EplImplPlatform
{
    struct
    {
        PFNEGLQUERYDISPLAYATTRIBKHRPROC QueryDisplayAttribKHR;
        PFNEGLSWAPINTERVALPROC SwapInterval;
        PFNEGLQUERYDMABUFFORMATSEXTPROC QueryDmaBufFormatsEXT;
        PFNEGLQUERYDMABUFMODIFIERSEXTPROC QueryDmaBufModifiersEXT;
        PFNEGLCREATESYNCPROC CreateSync;
        PFNEGLDESTROYSYNCPROC DestroySync;
        PFNEGLWAITSYNCPROC WaitSync;
        PFNEGLDUPNATIVEFENCEFDANDROIDPROC DupNativeFenceFDANDROID;
        void (* Flush) (void);
        void (* Finish) (void);

        pfn_eglPlatformImportColorBufferNVX PlatformImportColorBufferNVX;
        pfn_eglPlatformFreeColorBufferNVX PlatformFreeColorBufferNVX;
        pfn_eglPlatformCreateSurfaceNVX PlatformCreateSurfaceNVX;
        pfn_eglPlatformSetColorBuffersNVX PlatformSetColorBuffersNVX;
        pfn_eglPlatformGetConfigAttribNVX PlatformGetConfigAttribNVX;
        pfn_eglPlatformCopyColorBufferNVX PlatformCopyColorBufferNVX;
        pfn_eglPlatformAllocColorBufferNVX PlatformAllocColorBufferNVX;
        pfn_eglPlatformExportColorBufferNVX PlatformExportColorBufferNVX;
    } egl;

    struct
    {
        int (* GetDeviceFromDevId) (dev_t dev_id, uint32_t flags, drmDevicePtr *device);
        int (* GetCap) (int fd, uint64_t capability, uint64_t *value);
        int (* SyncobjCreate) (int fd, uint32_t flags, uint32_t *handle);
        int (* SyncobjDestroy) (int fd, uint32_t handle);
        int (* SyncobjHandleToFD) (int fd, uint32_t handle, int *obj_fd);
        int (* SyncobjFDToHandle) (int fd, int obj_fd, uint32_t *handle);
        int (* SyncobjImportSyncFile) (int fd, uint32_t handle, int sync_file_fd);
        int (* SyncobjExportSyncFile) (int fd, uint32_t handle, int *sync_file_fd);

        int (* SyncobjTimelineSignal) (int fd, const uint32_t *handles,
                            uint64_t *points, uint32_t handle_count);
        int (* SyncobjTimelineWait) (int fd, uint32_t *handles, uint64_t *points,
                          unsigned num_handles,
                          int64_t timeout_nsec, unsigned flags,
                          uint32_t *first_signaled);
        int (* SyncobjTransfer) (int fd,
                          uint32_t dst_handle, uint64_t dst_point,
                          uint32_t src_handle, uint64_t src_point,
                          uint32_t flags);
    } drm;

    struct
    {
        struct wl_event_queue * (* display_create_queue_with_name) (
                struct wl_display *display, const char *name);
    } wl;

    EGLBoolean timeline_funcs_supported;
};

/**
 * Finds an EGLDeviceEXT handle that corresponds to a given DRI device node.
 */
EGLDeviceEXT eplWlFindDeviceForNode(EplPlatformData *plat, const char *node);

/**
 * A wrapper around the DMA_BUF_IOCTL_IMPORT_SYNC_FILE ioctl.
 *
 * \param dmabuf The dma-buf to modify.
 * \param syncfd The sync file to attach as the write fence.
 *
 * \return EGL_TRUE on success, or EGL_FALSE on failure.
 */
EGLBoolean eplWlImportDmaBufSyncFile(int dmabuf, int syncfd);

/**
 * A wrapper around the DMA_BUF_IOCTL_EXPORT_SYNC_FILE ioctl.
 *
 * \param dmabuf The dma-buf to get a fence from.
 *
 * \return The sync file for the read fence, or -1 on failure.
 */
int eplWlExportDmaBufSyncFile(int dmabuf);

EGLSurface eplWlCreateWindowSurface(EplPlatformData *plat, EplDisplay *pdpy, EplSurface *psurf,
        EGLConfig config, void *native_surface, const EGLAttrib *attribs, EGLBoolean create_platform,
        const struct glvnd_list *existing_surfaces);

void eplWlDestroyWindow(EplDisplay *pdpy, EplSurface *psurf,
            const struct glvnd_list *existing_surfaces);

EGLBoolean eplWlSwapBuffers(EplPlatformData *plat, EplDisplay *pdpy,
        EplSurface *psurf, const EGLint *rects, EGLint n_rects);

EGLBoolean eplWlSwapInterval(EplDisplay *pdpy, EplSurface *psurf, EGLint interval);

EGLBoolean eplWlWaitGL(EplDisplay *pdpy, EplSurface *psurf);

#endif // WAYLAND_PLATFORM_H
