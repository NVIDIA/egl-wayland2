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

#include "wl-object-utils.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

EGLBoolean wlEglMemoryIsReadable(const void *p, size_t len)
{
    int fds[2], result = -1;

    /*
     * If the address is below some small-ish value, then assume it's not
     * readable. This is mainly useful as an early-out when we're trying to
     * figure out if a wl_egl_window starts with a version number or a
     * wl_surface.
     */
    if (((uintptr_t) p) < 256)
    {
        return EGL_FALSE;
    }

    if (pipe(fds) == -1) {
        return EGL_FALSE;
    }

    if (fcntl(fds[1], F_SETFL, O_NONBLOCK) == -1) {
        goto done;
    }

    /* write will fail with EFAULT if the provided buffer is outside
     * our accessible address space. */
    result = write(fds[1], p, len);
    assert(result != -1 || errno == EFAULT);

done:
    close(fds[0]);
    close(fds[1]);
    return result != -1;
}

EGLBoolean wlEglCheckInterfaceType(struct wl_object *obj, const char *ifname)
{
    /* The first member of a wl_object is a pointer to its wl_interface, */
    struct wl_interface *interface;
    size_t len;

    if (!wlEglMemoryIsReadable(obj, sizeof(void *)))
    {
        return EGL_FALSE;
    }

    interface = *(void **)obj;

    /* Check if the memory for the wl_interface struct, and the
     * interface name, are safe to read. */
    len = strlen(ifname);
    if (!wlEglMemoryIsReadable(interface, sizeof (*interface))
            || !wlEglMemoryIsReadable(interface->name, len + 1))
    {
        return EGL_FALSE;
    }

    return !strcmp(interface->name, ifname);
}

/**
 * Returns the version number and the wl_surface pointer from a wl_egl_window.
 */
EGLBoolean wlEglGetWindowVersionAndSurface(struct wl_egl_window *window,
        long int *ret_version, struct wl_surface **ret_surface)
{
    long int version = 0;
    struct wl_surface *surface = NULL;

    if (window == NULL || !wlEglMemoryIsReadable(window, sizeof (*window)))
    {
        return EGL_FALSE;
    }

    /*
     * Given that wl_egl_window wasn't always a versioned struct, and that
     * 'window->version' replaced 'window->surface', we must check whether
     * 'window->version' is actually a valid pointer. If it is, we are dealing
     * with a wl_egl_window from an old implementation of libwayland-egl.so
     */

    if (wlEglCheckInterfaceType((struct wl_object *) window->version, "wl_surface"))
    {
        version = 0;
        surface = (struct wl_surface *) window->version;
    }
    else if (wlEglCheckInterfaceType((struct wl_object *) window->surface, "wl_surface"))
    {
        version = window->version;
        surface = window->surface;
    }
    else
    {
        return EGL_FALSE;
    }

    *ret_version = version;
    *ret_surface = surface;
    return EGL_TRUE;
}

