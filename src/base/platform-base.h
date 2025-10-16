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

#ifndef PLATFORM_BASE_H
#define PLATFORM_BASE_H

/**
 * \file
 *
 * Common bookkeeping and infrastructure for an EGL platform library.
 *
 * These functions handle the basic tasks of keeping track of internal and
 * external EGLDisplays and EGLSurfaces.
 */

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <pthread.h>

#include <eglexternalplatform.h>

#include "glvnd_list.h"
#include "refcountobj.h"

#define PUBLIC __attribute__((visibility("default")))

#ifdef __cplusplus
extern "C" {
#endif

// Opaque types for private implementation data.
typedef struct _EplImplSurface EplImplSurface;
typedef struct _EplImplDisplay EplImplDisplay;
typedef struct _EplImplPlatform EplImplPlatform;

typedef enum
{
    EPL_SURFACE_TYPE_WINDOW,
    EPL_SURFACE_TYPE_PIXMAP,
} EplSurfaceType;

/**
 * Keeps track of an internal EGLDisplay.
 */
typedef struct
{
    EplRefCount refcount;

    EGLDisplay edpy;

    /**
     * The number of times that this display has been initialized. This is used
     * to simulate the EGL_KHR_display_reference extension even if the
     * underlying driver doesn't support it.
     */
    unsigned int init_count;
    EGLint major;
    EGLint minor;

    struct glvnd_list entry;
} EplInternalDisplay;

/**
 * Keeps track of an EGLSurface.
 */
typedef struct
{
    EGLSurface external_surface;
    EGLSurface internal_surface;
    EplSurfaceType type;

    /**
     * Private data used by the implementation.
     */
    EplImplSurface *priv;

    /**
     * True if eglSetDamageRegionKHR has been called since the last
     * eglSwapBuffers.
     */
    EGLBoolean setDamageRegionCalled;

    /**
     * True EGL_BUFFER_AGE has been queried since the last eglSwapBuffers.
     */
    EGLBoolean bufferAgeCalled;

    struct glvnd_list entry;
} EplSurface;

/**
 * Keeps track of data for an external (application-facing) EGLDisplay.
 */
typedef struct
{
    /**
     * A reference count. This is used so that we know when it's safe to free
     * EplDisplay struct.
     *
     * Since EGLDisplays can't be destroyed (yet), this only really matters if
     * we go through teardown while another thread is still using the
     * EplDisplay. It'll be more interesting once we add support for
     * EGL_EXT_display_alloc.
     */
    EplRefCount refcount;

    /**
     * The external (application-facing) EGLDisplay handle.
     */
    EGLDisplay external_display;

    /**
     * The internal EGLDisplay handle.
     */
    EGLDisplay internal_display;

    /**
     * The platform enum (EGL_PLATFORM_X11_KHR, etc.).
     */
    EGLenum platform_enum;

    /**
     * The native display that this EplDisplay was created from.
     */
    void *native_display;

    /**
     * A pointer back to the EplPlatformData struct that owns this EplDisplay.
     *
     * This is needed because most of the hook functions don't get a separate
     * parameter for the EplPlatformData.
     */
    struct _EplPlatformData *platform;

    /**
     * Private data for the implementation.
     */
    EplImplDisplay *priv;

    // Everything after this in EplDisplay should be treated as internal to
    // platform-base.c.

	/**
	 * A read/write lock to protect against concurrent calls to eglTerminate.
	 *
	 * eglInitialize, eglTerminate, and library teardown will all take the read
	 * lock.
	 *
	 * All other functions will take the read lock, so that they don't have to
	 * worry about another thread coming along and terminating the display.
	 */
	pthread_rwlock_t init_lock;

    /**
     * All of the existing EplSurface structs.
     */
    struct glvnd_list surface_list;

    /**
     * A read/write lock to protect the surface list.
     *
     * The eglCreate*Surface and eglDestroySurface hooks will hold the write
     * lock, and any other functions that operate on an EGLSurface a will hold
     * the read lock.
     *
     * This allows most functions (especially eglSwapBuffers, which may have to
     * block for extended periods of time) to run concurrently, without needing
     * to worry about the EGLSurface getting destroyed out from under it.
     */
    pthread_rwlock_t surface_list_lock;

    /**
     * True if this display was created with EGL_TRACK_REFERENCES set.
     */
    EGLBoolean track_references;

    /**
     * The number of times that the display has been initialized. If this
     * display was not created with EGL_TRACK_REFERENCES set, then this is
     * capped at 1.
     */
    unsigned int init_count;

    /// The major version number for eglInitialize in this context.
    EGLint major;
    /// The minor version number for eglInitialize in this context.
    EGLint minor;
    /// True if this display has been initialized.
    EGLBoolean initialized;

    struct glvnd_list entry;
} EplDisplay;

typedef struct _EplPlatformData
{
    EplRefCount refcount;

    struct {
        PFNEGLQUERYSTRINGPROC QueryString;
        PFNEGLGETPLATFORMDISPLAYPROC GetPlatformDisplay;
        PFNEGLINITIALIZEPROC Initialize;
        PFNEGLTERMINATEPROC Terminate;
        PFNEGLGETERRORPROC GetError;
        PFNEGLCREATEPBUFFERSURFACEPROC CreatePbufferSurface;
        PFNEGLDESTROYSURFACEPROC DestroySurface;
        PFNEGLSWAPBUFFERSPROC SwapBuffers;
        PFNEGLCHOOSECONFIGPROC ChooseConfig;
        PFNEGLGETCONFIGATTRIBPROC GetConfigAttrib;
        PFNEGLGETCONFIGSPROC GetConfigs;
        PFNEGLGETCURRENTDISPLAYPROC GetCurrentDisplay;
        PFNEGLGETCURRENTSURFACEPROC GetCurrentSurface;
        PFNEGLGETCURRENTCONTEXTPROC GetCurrentContext;
        PFNEGLMAKECURRENTPROC MakeCurrent;
        PFNEGLWAITGLPROC WaitGL;
        PFNEGLWAITCLIENTPROC WaitClient;
        PFNEGLWAITNATIVEPROC WaitNative;
        PFNEGLSWAPINTERVALPROC SwapInterval;
        PFNEGLQUERYSURFACEPROC QuerySurface;

        PFNEGLQUERYDEVICEATTRIBEXTPROC QueryDeviceAttribEXT;
        PFNEGLQUERYDEVICESTRINGEXTPROC QueryDeviceStringEXT;
        PFNEGLQUERYDEVICESEXTPROC QueryDevicesEXT;
        PFNEGLQUERYDISPLAYATTRIBEXTPROC QueryDisplayAttribEXT;

        PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC SwapBuffersWithDamage;
        PFNEGLCREATESTREAMPRODUCERSURFACEKHRPROC CreateStreamProducerSurfaceKHR;

        PFNEGLSETDAMAGEREGIONKHRPROC SetDamageRegionKHR;
    } egl;

    struct
    {
        EGLBoolean display_reference;
    } extensions;

    struct
    {
        PEGLEXTFNGETPROCADDRESS getProcAddress;
        PEGLEXTFNDEBUGMESSAGE debugMessage;
        PEGLEXTFNSETERROR setError;
    } callbacks;

    /**
     * True if we're going through teardown for this platform. Once we're in
     * teardown, it's no longer safe to call into the driver.
     *
     * Note that if another thread is currently calling an EGL function when
     * the platform library gets torn down, then things are likely to break no
     * matter what, because the driver will have finished a lot of its teardown
     * before the platform library finds out about it.
     *
     * Thus, this flag is only to make it easier to share cleanup code between
     * platform library teardown and eglDestroySurface et. al.
     */
    EGLBoolean destroyed;

    /**
     * Private data for the implementation.
     */
    EplImplPlatform *priv;

    struct glvnd_list internal_display_list;
    pthread_mutex_t internal_display_list_mutex;

    EGLenum platform_enum;
    const struct _EplImplFuncs *impl;

    struct glvnd_list entry;
} EplPlatformData;

EPL_REFCOUNT_DECLARE_TYPE_FUNCS(EplPlatformData, eplPlatformData);
EPL_REFCOUNT_DECLARE_TYPE_FUNCS(EplInternalDisplay, eplInternalDisplay);

/**
 * Allocates and initializes an EplPlatformData struct.
 *
 * This is called from the loadEGLExternalPlatform entrypoint.
 *
 * After calling eplPlatformBaseAllocate, the caller should perform any
 * platform-specific initialization, and then call eplPlatformBaseInitFinish
 * (on success) or eplPlatformBaseInitFail (on failure).
 *
 * \param platform_enum The EGL enum value for this platform.
 * \param impl The platform implementation functions.
 * \param platform_priv_size If non-zero, then allocate additional space and
 *      assign it to EplPlatformData::priv.
 * \return A EplPlatformData struct, or NULL on error.
 */
EplPlatformData *eplPlatformBaseAllocate(int major, int minor,
        const EGLExtDriver *driver, EGLExtPlatform *extplatform,
        EGLenum platform_enum, const struct _EplImplFuncs *impl,
        size_t platform_priv_size);

/**
 * Finishes initializing a platform.
 *
 * This function should be called from loadEGLExternalPlatform after any
 * platform-specific initialization.
 */
void eplPlatformBaseInitFinish(EplPlatformData *plat);

/**
 * Cleans up a EplPlatformData after an init failure.
 *
 * This function should be called from loadEGLExternalPlatform if the
 * platform-specicic initialization fails.
 */
void eplPlatformBaseInitFail(EplPlatformData *plat);

/**
 * Looks up an EplDisplay struct.
 *
 * This will look up the display, lock it, and check to make sure that it's
 * initialized.
 *
 * The caller must call eplDisplayRelease to unlock and release the display.
 */
EplDisplay *eplDisplayAcquire(EGLDisplay edpy);

/**
 * Returns the current external EGLDisplay and EGLSurface handles for the
 * calling thread.
 *
 * \param[out] ret_edpy Returns the current external EGLDisplay
 * \param[out] ret_esurf Returns the current external EGLSurface
 */
void eplGetCurrentSurface(EGLDisplay *ret_edpy, EGLSurface *ret_esurf);

/**
 * Releases a display acquired with eplDisplayAcquire.
 */
void eplDisplayRelease(EplDisplay *pdpy);

/**
 * Looks up an internal EGLDisplay. If an EplInternalDisplay struct doesn't
 * already exist, then it will be created and returned.
 */
EplInternalDisplay *eplLookupInternalDisplay(EplPlatformData *platform, EGLDisplay handle);

/**
 * Returns an EplInternalDisplay struct for a device.
 *
 * This is just a convenience wrapper which creates an EGLDisplay from the
 * device and then calls eplLookupInternalDisplay.
 */
EplInternalDisplay *eplGetDeviceInternalDisplay(EplPlatformData *platform, EGLDeviceEXT dev);

/**
 * Calls eglInitialize on an internal display.
 */
EGLBoolean eplInitializeInternalDisplay(EplPlatformData *platform,
        EplInternalDisplay *idpy, EGLint *major, EGLint *minor);

/**
 * Calls eglTerminate on an internal display.
 */
EGLBoolean eplTerminateInternalDisplay(EplPlatformData *platform, EplInternalDisplay *idpy);

/**
 * Sets the current EGL error, and issues a debug message.
 */
void eplSetError(EplPlatformData *platform, EGLint error, const char *fmt, ...);

/**
 * Returns the display's surface list.
 *
 * This will take the read lock for the surface list, and then return the list
 * head.
 *
 * The caller must call eplDisplayUnlockSurfaceList to unlock the surface list
 * afterward.
 */
const struct glvnd_list *eplDisplayLockSurfaceList(EplDisplay *pdpy);

/**
 * Unlocks the display's surface list.
 */
void eplDisplayUnlockSurfaceList(EplDisplay *pdpy);

/**
 * A convenience function for hooks that operate on an EGLSurface.
 *
 * This function just calls eglDisplayAcquire to look up the EplDisplay, and
 * then eplDisplayLockSurfaceList and eplSurfaceListLookup to look up an
 * EplSurface.
 *
 * The caller must then call \c eplHookDisplaySurfaceEnd afterward.
 *
 * \note If \p esurf is not NULL, but doesn't match any EplSurface struct, then
 * this function will still succeed. In most cases, that just means that the
 * hook function should pass it through to the driver.
 *
 * \note If \p ret_psurf returns NULL, then the surface list will be left
 * unlocked.
 *
 * \param edpy The external EGLDisplay handle.
 * \param esurf The external EGLSurface handle.
 * \param[out] ret_pdpy Returns the EplDisplay pointer.
 * \param[out] ret_psurf Returns the EplSurface pointer, or NULL if \p esurf
 *      doesn't match any EplDisplay.
 * \return EGL_TRUE on success. EGL_FALSE if \p edpy is invalid, or if
 *      \p esurf is EGL_NO_SURFACE.
 */
EGLBoolean eplHookDisplaySurface(EGLDisplay edpy, EGLSurface esurf,
        EplDisplay **ret_pdpy, EplSurface **ret_surface);

void eplHookDisplaySurfaceEnd(EplDisplay *pdpy, const EplSurface *psurf);

/**
 * Looks up an EplSurface from its external EGLSurface handle.
 *
 * \param surface_list The surface list, as returned by \c eplDisplayLockSurfaceList.
 * \param esurf The external EGLSurface handle.
 * \return The corresponding EplSurface, or NULL if \p esurf doesn't match any
 *      surface.
 */
EplSurface *eplSurfaceListLookup(const struct glvnd_list *surface_list, EGLSurface esurf);

/**
 * Replaces the current surface.
 *
 * If \p old_surface is the current surface, then this will call eglMakeCurrent
 * to switch to \p new_surface.
 *
 * This is used to deal with stuff like window resizing, where we might need to
 * replace the internal EGLSurface handle for a surface.
 */
EGLBoolean eplSwitchCurrentSurface(EplPlatformData *platform, EplDisplay *pdpy,
        EGLSurface old_surface, EGLSurface new_surface);

/**
 * Returns a NULL-terminated array of all available EGLDeviceEXT handles.
 *
 * The caller must free the array using free().
 */
EGLDeviceEXT *eplGetAllDevices(EplPlatformData *platform, EGLint *ret_count);

/**
 * Locks and returns the list of EplDisplay structs.
 *
 * This can be used to deal with the application closing a native display out
 * from under us.
 *
 * The caller must call eplUnlockDisplayList after it's finished.
 */
struct glvnd_list *eplLockDisplayList(void);

void eplUnlockDisplayList(void);

#ifdef __cplusplus
}
#endif

#endif // PLATFORM_BASE_H
