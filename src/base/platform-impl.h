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

#ifndef PLATFORM_IMPL_H
#define PLATFORM_IMPL_H

/**
 * \file
 *
 * Functions that the platform implementation must implement.
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <eglexternalplatform.h>

#include "platform-base.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A table of functions for the platform-specific implementation.
 */
typedef struct _EplImplFuncs
{
    /**
     * Cleans up the platform data.
     */
    void (* CleanupPlatform) (EplPlatformData *plat);

    /**
     * Handles the EGLExtPlatformExports::QueryString export.
     *
     * Note that \p pdpy will not be NULL. If the driver passes in an unknown
     * EGLDisplay, then the base library will simply return without calling
     * this function. That means that currently, there's no way to add an
     * extension to an EGLDisplay that the platform library doesn't own.
     *
     * \param pdpy The EplDisplay struct.
     */
    const char * (* QueryString) (EplPlatformData *plat, EplDisplay *pdpy, EGLExtPlatformString name);

    /**
     * Checks if a pointer looks like a valid native display for a platform.
     *
     * This function is optional. If it's NULL, then it's equivalent to simply
     * returning EGL_FALSE.
     *
     * \param plat The EplPlatformData struct.
     * \param plat The native display pointer.
     * \return EGL_TRUE if \p nativeDisplay looks like a valid native display.
     */
    EGLBoolean (* IsValidNativeDisplay) (EplPlatformData *plat, void *nativeDisplay);

    /**
     * Returns the hook function for an EGL function.
     *
     * This function is optional. If it's NULL, then there are no
     * platform-specific hook functions.
     *
     * \param plat The EplPlatformData struct.
     * \param name The EGL function name.
     * \return A function pointer, or NULL if there isn't aa hook for that
     * function.
     */
    void * (* GetHookFunction) (EplPlatformData *plat, const char *name);

    /**
     * Checks if an eglGetPlatformDisplay call matches an existing EGLDisplay.
     *
     * Two eglGetPlatformDisplay calls with the same parameters are supposed to
     * return the same EGLDisplay. The base library checks the platform enum,
     * the native display pointer, and any attributes that the base library
     * itself handles.
     *
     * If there are additional platform-specific attributes, then this function
     * checks whether those attributes match an existing display, possibly
     * taking into account any default attribute values.
     *
     * If the implementation doesn't recognize an attribute, it may either
     * ignore the attribute or return EGL_FALSE.
     *
     * This function is optional. If it's NULL, then the platform does not
     * accept any platform-specific attributes, and so only the base library
     * checks described above apply.
     *
     * \param plat The EplPlatformData struct.
     * \param pdpy An existing EplDisplay struct to check.
     * \param platform The platform enum.
     * \param native_display The native display pointer.
     * \param attribs The remaining attributes. This array does not include
     *      the attributes that the base library handles.
     * \return EGL_TRUE if the attributes match \p pdpy, and so
     *      eglGetPlatformDisplay should return the EGLDisplay handle for it.
     */
    EGLBoolean (* IsSameDisplay) (EplPlatformData *plat, EplDisplay *pdpy, EGLint platform,
            void *native_display, const EGLAttrib *attribs);

    /**
     * Called to implement eglGetPlatformDisplay.
     *
     * \param plat The EplPlatformData struct.
     * \param pdpy The base EplDisplay struct. This will be filled in already,
     *      except for the \c internal_display member.
     * \param platform The platform enum.
     * \param native_display The native display pointer.
     * \param attribs The remaining attributes. This array does not include
     *      the attributes that the base library handles.
     *
     * \return EGL_TRUE on success, or EGL_FALSE on failure.
     */
    EGLBoolean (* GetPlatformDisplay) (EplPlatformData *plat, EplDisplay *pdpy,
            void *native_display, const EGLAttrib *attribs,
            struct glvnd_list *existing_displays);

    /**
     * Cleans up any implementation data in an EplDisplay.
     *
     * Currently, this is only called during teardown, but if/when
     * EGL_EXT_display_alloc is available, then this would be called to handle
     * destroying an EGLDisplay.
     *
     * If this is called during teardown, then the \c EplDisplay::platform pointer
     * may be NULL.
     *
     * Note that if a EGLDisplay is still initialized during teardown, then the
     * base library will call \c TerminateDisplay before \c CleanupDisplay.
     *
     * \param pdpy The EplDisplay struct to destroy.
     */
    void (* CleanupDisplay) (EplDisplay *pdpy);

    /**
     * Called to implement eglInitialize.
     *
     * Note that the base library handles EGL_KHR_display_reference, so this
     * function is only ever called on an uninitialized display.
     *
     * \param plat The EplPlatformData struct.
     * \param pdpy The EplDisplay to initialize.
     * \param[out] major Returns the major version number.
     * \param[out] minor Returns the minor version number.
     * \return EGL_TRUE on success, EGL_FALSE on failure.
     */
    EGLBoolean (* InitializeDisplay) (EplPlatformData *plat, EplDisplay *pdpy, EGLint *major, EGLint *minor);

    /**
     * Called to implement eglTerminate.
     *
     * Note that the base library handles EGL_KHR_display_reference, so this
     * function is only ever called on an initialized display.
     */
    void (* TerminateDisplay) (EplPlatformData *plat, EplDisplay *pdpy);

    /**
     * Creates an EGLSurface for a window.
     *
     * This function is optional. If it's NULL, then the platform does not
     * support window surfaces.
     *
     * \param plat The EplPlatformData struct
     * \param pdpy The EplDisplay struct
     * \param psurf An EplSurface struct, with the base information filled in.
     * \param native_surface The native surface handle.
     * \param attribs The attribute list.
     * \param create_platform If this is true, then the call is from
     *      eglCreatePlatformWindowSurface. If false, it's from
     *      eglCreateWindowSurface.
     * \param existing_surfaces A linked list of existing surfaces. The new
     *      surface will not be in this list.
     * \return The internal EGLSurface handle, or EGL_NO_SURFACE on failure.
     */
    EGLSurface (* CreateWindowSurface) (EplPlatformData *plat, EplDisplay *pdpy, EplSurface *psurf,
            EGLConfig config, void *native_surface, const EGLAttrib *attribs, EGLBoolean create_platform,
            const struct glvnd_list *existing_surfaces);

    /**
     * Creates an EGLSurface for a pixmap.
     *
     * This function is optional. If it's NULL, then the platform does not
     * support pixmap surfaces.
     *
     * \param plat The EplPlatformData struct
     * \param pdpy The EplDisplay struct
     * \param psurf An EplSurface struct, with the base information filled in.
     * \param native_surface The native surface handle.
     * \param attribs The attribute list.
     * \param create_platform If this is true, then the call is from
     *      eglCreatePlatformPixmapSurface. If false, it's from
     *      eglCreatePixmapSurface.
     * \param existing_surfaces A linked list of existing surfaces. The new
     *      surface will not be in this list.
     * \return The internal EGLSurface handle, or EGL_NO_SURFACE on failure.
     */
    EGLSurface (* CreatePixmapSurface) (EplPlatformData *plat, EplDisplay *pdpy, EplSurface *psurf,
            EGLConfig config, void *native_surface, const EGLAttrib *attribs, EGLBoolean create_platform,
            const struct glvnd_list *existing_surfaces);

    /**
     * Called from eglDestroySurface and eglTerminate to destroy a surface.
     *
     * After this, the \c EplSurface struct itself is freed.
     *
     * Note that this function is called with the surface list already locked,
     * so it must not try to call \c eplDisplayLockSurfaceList.
     *
     * \param pdpy The EplDisplay struct
     * \param psurf The EplSurface that's being destroyed.
     * \param existing_surfaces A linked list of existing surfaces. \p psurf
     *      will not be in this list.
     */
    void (* DestroySurface) (EplDisplay *pdpy, EplSurface *psurf,
            const struct glvnd_list *existing_surfaces);

    /**
     * Implements eglSwapBuffers and eglSwapBuffersWithDamageEXT.
     *
     * If the application calls eglSwapBuffers, then \p rects will be NULL and
     * \p n_rects will be zero.
     *
     * \param plat The EplPlatformData struct
     * \param pdpy The EplDisplay struct
     * \param psurf The EplSurface struct. This will always be the thread's
     *      current drawing surface.
     * \param rects The damage rectangles, or NULL.
     * \param n_rects The number of damage rectangles.
     * \return EGL_TRUE on success, EGL_FALSE on failure.
     */
    EGLBoolean (* SwapBuffers) (EplPlatformData *plat, EplDisplay *pdpy, EplSurface *psurf,
            const EGLint *rects, EGLint n_rects);

    /**
     * Implements eglWaitGL and eglWaitClient.
     *
     * This function is optional. If it's NULL, then the base library will
     * not provide a hook function eglWaitGL or eglWaitClient, and so the
     * driver will follow its default behavior.
     *
     * \param pdpy The EplDisplay struct
     * \param psurf The current EplSurface, or NULL if there isn't a current
     *      surface.
     * \return EGL_TRUE on success, EGL_FALSE on failure.
     */
    EGLBoolean (*WaitGL) (EplDisplay *pdpy, EplSurface *psurf);

    /**
     * Implements eglWaitNative.
     *
     * This function is optional. If it's NULL, then the base library will not
     * provide a hook function eglWaitNative, and so the driver will follow its
     * default behavior.
     *
     * \param pdpy The EplDisplay struct
     * \param psurf The current EplSurface, or NULL if there isn't a current
     *      surface.
     * \return EGL_TRUE on success, EGL_FALSE on failure.
     */
    EGLBoolean (*WaitNative) (EplDisplay *pdpy, EplSurface *psurf);

    /**
     * Implements eglQueryDisplayAttribKHR/EXT/NV.
     *
     * This function is optional, if it's NULL, then the base library will
     * handle any attributes that it implements internally, and forward the
     * rest to the driver.
     *
     * If the platform doesn't recognize \p attrib, then it should forward the
     * function to the driver.
     *
     * \param pdpy The EplDisplay struct
     * \param attrib The attribute to look up.
     * \param[out] value Returns the value of the attribute.
     * \return EGL_TRUE on success, EGL_FALSE on failure.
     */
    EGLBoolean (*QueryDisplayAttrib) (EplDisplay *pdpy, EGLint attrib, EGLAttrib *ret_value);

    /**
     * Implements eglSwapInterval.
     *
     * This is only called if the current EGLSurface belongs to the platform
     * library. If the current EGLSurface does not belong to the platform
     * library (e.g., a pbuffer or stream), then the base library will pass the
     * call through to the driver.
     *
     * This function is optional. If it's NULL, then the base library will not
     * provide a hook function eglSwapInterval, and so the driver will follow its
     * default behavior.
     *
     * \param pdpy The current display.
     * \param psurf The current draw surface. This will never be NULL.
     * \param interval The new swap interval.
     * \return EGL_TRUE on success, or EGL_FALSE on failure.
     */
    EGLBoolean (* SwapInterval) (EplDisplay *pdpy, EplSurface *psurf, EGLint interval);

    /**
     * Returns the value of EGL_BUFFER_AGE_KHR.
     *
     * This function is optional. If it's NULL, then the base library will just
     * return zero.
     *
     * \param pdpy The current display.
     * \param psurf The current draw surface. This will never be NULL.
     * \return The buffer age, or -1 on error.
     */
    EGLint (* QueryBufferAge) (EplDisplay *pdpy, EplSurface *psurf);
} EplImplFuncs;

#ifdef __cplusplus
}
#endif
#endif // PLATFORM_IMPL_H
