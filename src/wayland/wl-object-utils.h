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

#ifndef WL_OBJECT_UTILS_H
#define WL_OBJECT_UTILS_H

/**
 * \file
 *
 * Some functions for dealing with Wayland object structs.
 */

#include <stdint.h>
#include <sys/types.h>

#include <wayland-client-core.h>
#include <wayland-egl-backend.h>

#include <EGL/egl.h>

/**
 * Returns EGL_TRUE if memory at a given pointer is readable.
 *
 * \param p The pointer to check.
 * \param len The number of bytes that must be readable.
 *
 * \return EGL_TRUE if the memory is readable, EGL_FALSE otherwise.
 */
EGLBoolean wlEglMemoryIsReadable(const void *p, size_t len);

/**
 * Returns EGL_TRUE if the given wl_object is a particular type.
 *
 * This will use \c wlEglMemoryIsReadable to make sure that memory is readable
 * before it tries to dereference anything.
 *
 * \param obj A pointer to a wl_object.
 * \param ifname The name of the Wayland interface.
 *
 * \return EGL_TRUE if \p obj appears to be a valid pointer to a Wayland
 *      object with interface \p ifname.
 */
EGLBoolean wlEglCheckInterfaceType(struct wl_object *obj, const char *ifname);

/**
 * Returns the version number and the wl_surface pointer from a wl_egl_window.
 *
 * This function will check if \p window is a valid \c wl_egl_window, and then
 * figure out what version it is.
 *
 * \param window A pointer to a wl_egl_window.
 * \param[out] ret_version Returns the version number of the struct.
 * \param[out] ret_surface Returns the wl_surface proxy.
 *
 * \return EGL_TRUE if \p window was a valid \c wl_egl_window, or EGL_FALSE if
 * it was invalid.
 */
EGLBoolean wlEglGetWindowVersionAndSurface(struct wl_egl_window *window,
        long int *ret_version, struct wl_surface **ret_surface);

#endif // WL_OBJECT_UTILS_H
