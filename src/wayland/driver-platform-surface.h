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

/**
 * \file
 *
 * The platform surface interface for the NVIDIA driver.
 *
 * This interface provides a new EGLSurface which renders to caller-allocated
 * color buffers. Conceptually, it's similar to an FBO, but at the EGL level
 * instead of OpenGL.
 *
 * Note that this interface is still somewhat experimental, and might change in
 * backwards-incompatible ways.
 */

#ifndef DRIVER_PLATFORM_SURFACE_H
#define DRIVER_PLATFORM_SURFACE_H

#include <EGL/egl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EGL_PLATFORM_SURFACE_UPDATE_CALLBACK_NVX ((EGLAttrib) 0x80000001U)
#define EGL_PLATFORM_SURFACE_UPDATE_CALLBACK_PARAM_NVX ((EGLAttrib) 0x80000002U)
#define EGL_PLATFORM_SURFACE_DAMAGE_CALLBACK_NVX ((EGLAttrib) 0x80000003U)
#define EGL_PLATFORM_SURFACE_DAMAGE_CALLBACK_PARAM_NVX ((EGLAttrib) 0x80000004U)
#define EGL_PLATFORM_SURFACE_BLIT_TARGET_NVX ((EGLAttrib) 0x80000005U)

/**
 * If this attribute is EGL_TRUE, then the surface will treat the origin as the
 * top-left corner (the convention used by e.g., x11).
 *
 * If it's EGL_FALSE, then the origin is in the bottom-left corner (the
 * convention used by OpenGL textures).
 *
 * We re-use the same value as EGL_WAYLAND_Y_INVERTED_WL for this, since it's
 * more-or-less the same meaning.
 */
#define EGL_SURFACE_Y_INVERTED_NVX 0x31DB

#define EGL_PLATFORM_SURFACE_INTERFACE_MAJOR_VERSION 0
#define EGL_PLATFORM_SURFACE_INTERFACE_MINOR_VERSION 1

static inline EGLint EGL_PLATFORM_SURFACE_INTERFACE_GET_MAJOR_VERSION(EGLint version)
{
    return version >> 16;
}
static inline EGLint EGL_PLATFORM_SURFACE_INTERFACE_GET_MINOR_VERSION(EGLint version)
{
    return version & 0xFFFF;
}

/**
 * Checks if the version number reported by the driver is compatible.
 *
 * \param driver_version The version number reported by \c eglPlatformGetVersionNVX.
 * \param major_version The major version number that the library expects.
 * \param min_minor_version The minimum minor version number that the library requires.
 * \return EGL_TRUE if the driver's version is compatible.
 */
static inline EGLBoolean EGL_PLATFORM_SURFACE_INTERFACE_CHECK_VERSION(EGLint driver_version,
        EGLint major_version, EGLint min_minor_version)
{
    return EGL_PLATFORM_SURFACE_INTERFACE_GET_MAJOR_VERSION(driver_version) == major_version
        && EGL_PLATFORM_SURFACE_INTERFACE_GET_MINOR_VERSION(driver_version) >= min_minor_version;
}

/**
 * An opaque handle to a color buffer.
 *
 * Note that a color buffer may only be attached to one attachment point of one
 * EGLSurface at a time.
 *
 * Detaching a buffer from one surface and then attaching it to another is
 * allowed, but may incur a performance cost (e.g., due to reallocating
 * multisample buffers).
 */
typedef struct EGLPlatformColorBufferNVXRec *EGLPlatformColorBufferNVX;

/**
 * A callback to update an EGLSurface. This is used for dealing with window
 * resizes and similar.
 *
 * Because this is called from within the driver, it's not safe to call any EGL
 * or OpenGL functions from this callback. The callback may only call functions
 * that are explicitly defined as safe. Calling any other function may result
 * in undefined behavior.
 *
 * This function is only ever called for the current EGLSurface, or from an
 * eglMakeCurrent call that's setting the EGLSurface to be current. Thus, the
 * platform library can safely assume that the EGLSurface will not be current
 * to any other thread, and that there will not be a concurrent call to
 * eglSwapBuffers for this surface.
 *
 * \param param The pointer that was passed as the
 *      \c EGL_PLATFORM_SURFACE_UPDATE_CALLBACK_PARAM_NVX attribute to
 *      \c eglPlatformCreateSurfaceNVX.
 */
typedef void (* EGLExtPlatformSurfaceUpdateCallback) (void *param);

/**
 * A callback to handle front- or single-buffered rendering for EGL platform
 * surfaces.
 *
 * For a double-buffered EGLSurface, this is called when the front buffer
 * changed.
 *
 * For a single-buffered EGLSurface, this is called when single buffer changed.
 *
 * In both cases, it's called after a flush, so whatever rendering happened is
 * in progress.
 *
 * The callback must not call back into the driver at all. Calling any
 * driver function, including the functions that would be safe from
 * \c EGLExtPlatformSurfaceUpdateCallback, may result in undefined behavior.
 *
 * As with \c EGLExtPlatformSurfaceDamageCallback, this function is only called
 * for the current thread's current surface. Therefore, the platform library
 * can assume that there will not be a concurrent call to eglSwapBuffers for
 * the same EGLSurface on any other thread.
 *
 * \note The driver will close \p syncfd after the callback returns. If the
 * platform library needs to hold onto it, then it must call dup().
 *
 * \param param A pointer to caller-specific data. This is the pointer passed
 *      as the EGL_PLATFORM_SURFACE_DAMAGE_CALLBACK_PARAM_NVX attribute to
 *      \c eglPlatformCreateSurfaceNVX.
 * \param syncfd A file descriptor for a binary fence, or -1. If this is -1,
 *      then the driver will have already waited for any rendering to finish.
 * \param flags Currently unused.
 */
typedef void (* EGLExtPlatformSurfaceDamageCallback) (void *param, int syncfd, unsigned int flags);

/**
 * Returns a version number for the platform surface interface.
 *
 * This version contains a major version number in the high-order 16 bits, and
 * a minor version number in the low-order 16 bits.
 *
 * The major version will change if there's ever an interface change that
 * would break backwards compatibility.
 *
 * The minor version number will be incremented for new, backwards-compatible
 * functions or features.
 *
 * Use \c EGL_PLATFORM_SURFACE_INTERFACE_GET_MAJOR_VERSION and
 * \c EGL_PLATFORM_SURFACE_INTERFACE_GET_MINOR_VERSION to extract the major
 * and minor version numbers.
 *
 * As the interface is not yet stable, neither backward nor forward
 * compatibility is guaranteed between different versions.
 */
typedef EGLint (* pfn_eglPlatformGetVersionNVX) (void);

/**
 * Creates an EGLPlatformColorBufferNVX from a dma-buf.
 *
 * After this function returns, the caller may close the file descriptor.
 *
 * Note that the caller must free the color buffer by calling
 * \c eglPlatformFreeColorBufferNVX. The driver will not free the color buffers
 * when the display is terminated.
 *
 * This function may be called from the update callback.
 *
 * \param dpy The internal EGLDisplay handle.
 * \param fd The dma-buf file descriptor to import.
 * \param width The width of the image.
 * \param height The height of the image.
 * \param format The fourcc format code.
 * \param stride The stride or pitch of the image.
 * \param offset The offset of the image.
 * \param modifier The format modifier of the image.
 *
 * \return A new EGLPlatformColorBufferNVX handle, or NULL on error.
 */
typedef EGLPlatformColorBufferNVX (* pfn_eglPlatformImportColorBufferNVX) (EGLDisplay dpy,
        int fd, int width, int height, int format, int stride, int offset,
        unsigned long long modifier);

/**
 * Allocates a color buffer.
 *
 * Note that for a simple renderable buffer, a platform library should
 * generally allocate the buffer using libgbm and then import it using
 * \c eglPlatformImportColorBufferNVX, because libgbm allows the driver to
 * select an optimal format modifier.
 *
 * However, this function can be used to allocate a buffer in system memory.
 *
 * The caller can use \c eglPlatformExportColorBufferNVX to export the buffer
 * as a dma-buf.
 *
 * This function may be called from the update callback.
 *
 * \param dpy The internal EGLDisplay handle.
 * \param width The width of the image.
 * \param height The height of the image.
 * \param format The fourcc format code.
 * \param modifier The format modifier of the image.
 * \param force_sysmem If true, then allocate the buffer in sysmem, not in
 *      vidmem.
 */
typedef EGLPlatformColorBufferNVX (* pfn_eglPlatformAllocColorBufferNVX) (EGLDisplay dpy,
        int width, int height, int format, unsigned long long modifier,
        EGLBoolean force_sysmem);

/**
 * Exports a color buffer as a dma-buf.
 *
 * This function returns a dma-buf file descriptor for a color buffer, along
 * with the necessary parameters to import it elsewhere.
 *
 * Any of the output parameters may be NULL if the caller does not require
 * them.
 *
 * \param dpy The internal EGLDisplay handle.
 * \param buffer The color buffer to export.
 * \param[out] ret_fd Returns the dma-buf file descriptor. The caller
 *      is responsible for closing it.
 * \param[out] ret_width Returns the width of the image.
 * \param[out] ret_height Returns the height of the image.
 * \param[out] ret_format Returns the fourcc format code of the image.
 * \param[out] ret_stride Returns the stride of the image.
 * \param[out] ret_offset Returns the offset of the image.
 * \param[out] ret_modifier Returns the format modifier of the image.
 *
 * \return EGL_TRUE on success, or EGL_FALSE on error.
 */
typedef EGLBoolean (* pfn_eglPlatformExportColorBufferNVX) (EGLDisplay dpy, EGLPlatformColorBufferNVX buffer,
        int *ret_fd, int *ret_width, int *ret_height, int *ret_format, int *ret_stride, int *ret_offset,
        unsigned long long *ret_modifier);

/**
 * Copies an image between two buffers.
 *
 * Currently, this function only supports copying to a pitch linear buffer.
 *
 * The copy operation occurs as part of the OpenGL command stream for the
 * current context, so any rendering that the current context did to \p src
 * will complete before the copy. The caller can use normal synchronization
 * functions (glFinish, EGLSync objects, etc) to wait for the copy to finish.
 *
 * This function may NOT be called from the update callback.
 *
 * \param dpy The internal EGLDisplay handle. The display must be current.
 * \param src The source buffer.
 * \param dst The destination buffer. Currently, this must be a pitch linear
 *      image.
 * \return EGL_TRUE on success, or EGL_FALSE on failure.
 */
typedef EGLBoolean (* pfn_eglPlatformCopyColorBufferNVX) (EGLDisplay dpy,
        EGLPlatformColorBufferNVX src,
        EGLPlatformColorBufferNVX dst);

/**
 * Frees a color buffer.
 *
 * If \p buffer is attached to an EGLSurface, then it will be freed when it is
 * detached, or when the EGLSurface is destroyed.
 *
 * This function may be called from the update callback.
 *
 * \param dpy The internal EGLDisplay handle.
 * \param buffer The EGLPlatformColorBufferNVX to free.
 */
typedef void (* pfn_eglPlatformFreeColorBufferNVX) (EGLDisplay dpy,
            EGLPlatformColorBufferNVX buffer);

/**
 * Creates a new platform surface.
 *
 * The color buffers are specified in the \p platformAttribs array, using
 * GL_FRONT and GL_BACK for the front and back buffer.
 *
 * The update and damage callbacks can be specified in the \p platformAttribs
 * as well.
 *
 * The \p platformAttribs array is a separate EGLAttrib array so that there
 * isn't any risk of conflict with existing or future EGL enums.
 *
 * The driver accepts the same set of EGLConfigs for platform surfaces that it
 * does for streams, so \p config must have EGL_STREAM_BIT_KHR set.
 *
 * This function may NOT be called from the update callback.
 *
 * \param dpy The internal EGLDisplay handle.
 * \param config The EGLConfig to use for the new surface
 * \param platformAttribs The color buffers and other platform library attributes.
 * \param attribs An array of other attributes.
 *
 * \return A new EGLSurface, or EGL_NO_SURFACE on error.
 */
typedef EGLSurface (* pfn_eglPlatformCreateSurfaceNVX) (EGLDisplay dpy, EGLConfig config,
        const EGLAttrib *platformAttribs, const EGLAttrib *attribs);

/**
 * Sets the color buffers for an EGLSurface.
 *
 * This function may be called from the update callback, but only for the
 * surface that's being updated.
 *
 * The buffers are specified in an EGLAttrib array, using the same attributes
 * as eglPlatformCreateSurfaceNVX.
 *
 * Outside the update callback, this function may only be called for the
 * current thread's current surface (e.g., from eglSwapBuffers). If the surface
 * is not current, then the result is undefined.
 *
 * The EGLSurface must remain single- or double-buffered. For example, if the
 * EGLSurface was created with only a back buffer, then the platform library
 * may not attach a front buffer using eglPlatformSetColorBuffersNVX.
 *
 * The platform library may, however, change the EGL_PLATFORM_SURFACE_BLIT_TARGET_NVX
 * attachment between NULL and non-NULL.
 *
 * This function may be called from the update callback.
 *
 * \param dpy The internal EGLDisplay handle.
 * \param surf The EGLSurface handle.
 * \param buffers The new buffers, terminated by EGL_NONE.
 *
 * \return EGL_TRUE on success, or EGL_FALSE on failure.
 */
typedef EGLBoolean (* pfn_eglPlatformSetColorBuffersNVX) (EGLDisplay dpy,
        EGLSurface surf, const EGLAttrib *buffers);

/**
 * Returns EGLConfig attributes.
 *
 * This function is equivalent to eglGetConfigAttrib, but with additional
 * attributes.
 *
 * Currently, this function supports \c EGL_LINUX_DRM_FOURCC_EXT, which returns
 * a fourcc format code for the EGLConfig, or DRM_FORMAT_INVALID if the config
 * doesn't have a corresponding fourcc code.
 *
 * \param dpy The internal EGLDisplay handle.
 * \param config The EGLConfig to query.
 * \param attribute The attribute to look up, which may be
 *      EGL_LINUX_DRM_FOURCC_EXT or any attribute that eglGetConfigAttrib
 *      supports.
 * \param[out] value Returns the attribute value.
 *
 * \return EGL_TRUE on success, or EGL_FALSE on error.
 */
typedef EGLBoolean (* pfn_eglPlatformGetConfigAttribNVX) (EGLDisplay dpy,
        EGLConfig config, EGLint attribute, EGLint *value);

#ifdef __cplusplus
}
#endif
#endif // DRIVER_PLATFORM_SURFACE_H
