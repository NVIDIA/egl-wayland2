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

#ifndef WAYLAND_FBCONFIG_H
#define WAYLAND_FBCONFIG_H

#include "wayland-platform.h"
#include "wayland-dmabuf.h"

/**
 * Looks up the formats that the driver supports for rendering.
 */
WlFormatList *eplWlGetDriverFormats(EplPlatformData *plat, EGLDisplay internal_display);

/**
 * Constructs the EGLConfig list for an EGLDisplay.
 *
 * \param plat The platform data
 * \param internal_display The internal EGLDisplay handle
 * \param tranches A list of WlDmaBufFeedbackTranche structs for the dma-buf
 *      feedback data.
 * \param render_devices The device nodes for the device that we're rendering on.
 * \param render_device_count The number of elements in \c render_devices.
 * \param driver_formats The list of formats that the driver supports, as
 *      returned by \c eplWlGetDriverFormats
 * \param allow_prime If true, then we can use PRIME, so treat pitch linear as
 *      supported.
 * \param from_init True if this is being called from eglInitialize. This
 *      affects error reporting.
 *
 * \return An EplConfigList, or NULL on error. This list will contain at least
 *      one EGLConfig that supports windows.
 */
EplConfigList *eplWlInitConfigList(EplPlatformData *plat,
        EGLDisplay internal_display,
        struct glvnd_list *tranches,
        const dev_t *render_devices,
        size_t render_device_count,
        const WlFormatList *driver_formats,
        EGLBoolean allow_prime,
        EGLBoolean from_init);

EGLBoolean eplWlHookChooseConfig(EGLDisplay edpy, EGLint const *attribs,
        EGLConfig *configs, EGLint configSize, EGLint *numConfig);

EGLBoolean eplWlHookGetConfigAttrib(EGLDisplay edpy, EGLConfig config,
        EGLint attribute, EGLint *value);

#endif // WAYLAND_FBCONFIG_H
