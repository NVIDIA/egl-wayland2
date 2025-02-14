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

#ifndef WAYLAND_DISPLAY_H
#define WAYLAND_DISPLAY_H

#include "wayland-platform.h"
#include "wayland-dmabuf.h"
#include "refcountobj.h"

#include <gbm.h>

#include "linux-drm-syncobj-v1-client-protocol.h"

/**
 * Contains data for an initialized EGLDisplay.
 */
typedef struct
{
    EplRefCount refcount;

    /**
     * The internal (driver) EGLDisplay.
     */
    EplInternalDisplay *internal_display;

    /**
     * A reference to the \c EplPlatformData that this display came from.
     *
     * This is mainly here so that we can access the driver's EGL functions
     * without going through an EplDisplay, since in some places (e.g., the
     * window update callback) might only have a WlDisplayInstance pointer.
     */
    EplPlatformData *platform;

    /**
     * The display connection.
     */
    struct wl_display *wdpy;

    /**
     * True if the application passed NULL for the native display, so we had to
     * open our own display connection.
     */
    EGLBoolean own_display;

    /**
     * Contains the global protocol objects that we need.
     */
    struct
    {
        struct zwp_linux_dmabuf_v1 *dmabuf;
        struct wp_linux_drm_syncobj_manager_v1 *syncobj;
    } globals;

    /**
     * The set of formats and modifiers that the server supports.
     */
    WlFormatList *default_feedback;

    /**
     * The set of formats and modifiers that the driver supports.
     */
    WlFormatList *driver_formats;

    EplConfigList *configs;

    /**
     * The GBM device for whichever GPU we're rendering on.
     */
    struct gbm_device *gbmdev;

    /**
     * The device ID for the render device.
     *
     * This is an array so that it contains both the primary and render
     * devices.
     */
    dev_t render_device_id[2];
    size_t render_device_id_count;

    /**
     * True if the driver supports the EGL_ANDROID_native_fence_sync extension.
     */
    EGLBoolean supports_EGL_ANDROID_native_fence_sync;

    /**
     * True if we can use implicit sync.
     */
    EGLBoolean supports_implicit_sync;
} WlDisplayInstance;

EPL_REFCOUNT_DECLARE_TYPE_FUNCS(WlDisplayInstance, eplWlDisplayInstance);

struct _EplImplDisplay
{
    /**
     * The EGLDeviceEXT handle that was specified with an EGL_DEVICE_EXT
     * attribute.
     */
    EGLDeviceEXT device_attrib;

    /**
     * The EGLDeviceEXT handle that we should use for rendering, or
     * EGL_NO_DEVICE_EXT to pick one during eglInitialize.
     *
     * This is set based on either the EGL_DEVICE_EXT attribute or based on
     * environment variables.
     */
    EGLDeviceEXT requested_device;

    /**
     * If true, allow picking a different GPU to do rendering.
     *
     * This is set based on the __NV_PRIME_RENDER_OFFLOAD environment variable.
     *
     * If the normal device (\c requested_device if it's set, the server's
     * device otherwise) isn't usable, then the \c enable_alt_device flag tells
     * eplWlDisplayInstanceCreate to pick a different device rather than just
     * fail.
     *
     * Note that this flag doesn't mean that we will use the PRIME presentation
     * path. It's possible that we'd pick the same device as the server anyway.
     *
     * Likewise, if the application passed an EGL_DISPLAY_EXT attribute, then
     * we might end up doing cross-device presentation even if the user doesn't
     * set __NV_PRIME_RENDER_OFFLOAD.
     */
    EGLBoolean enable_alt_device;

    /**
     * A pointer to the WlDisplayInstance struct, or NULL if this display isn't initialized.
     */
    WlDisplayInstance *inst;
};

EGLBoolean eplWlIsSameDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint platform,
        void *native_display, const EGLAttrib *attribs);
EGLBoolean eplWlGetPlatformDisplay(EplPlatformData *plat, EplDisplay *pdpy,
        void *native_display, const EGLAttrib *attribs,
        struct glvnd_list *existing_displays);
void eplWlCleanupDisplay(EplDisplay *pdpy);
EGLBoolean eplWlInitializeDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint *major, EGLint *minor);
void eplWlTerminateDisplay(EplPlatformData *plat, EplDisplay *pdpy);

#endif // WAYLAND_DISPLAY_H
