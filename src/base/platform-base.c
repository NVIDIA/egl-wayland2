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

#include "platform-base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

#include "platform-utils.h"
#include "platform-impl.h"

static void *eplGetHookAddressExport(void *platformData, const char *name);
static EGLBoolean eplIsValidNativeDisplayExport(void *platformData, void *nativeDisplay);
static EGLDisplay eplGetPlatformDisplayExport(void *platformData, EGLenum platform, void *nativeDisplay, const EGLAttrib* attribs);
static const char *eplQueryStringExport(void *platformData, EGLDisplay edpy, EGLExtPlatformString name);
static void *eplGetInternalHandleExport(EGLDisplay edpy, EGLenum type, void *handle);
static EGLBoolean eplUnloadExternalPlatformExport(void *platformData);

static void DeleteSurfaceCommon(EplDisplay *pdpy, EplSurface *psurf);
static EplSurface *AllocBaseSurface(EplPlatformData *plat);
static void FreeBaseSurface(EplSurface *psurf);

static void TerminateDisplay(EplDisplay *pdpy);
static void CheckTerminateDisplay(EplDisplay *pdpy);
static void DestroyDisplay(EplDisplay *pdpy);

/**
 * A list of all EplDisplay structs.
 */
static struct glvnd_list display_list = { &display_list, &display_list };
static pthread_mutex_t display_list_mutex;

/**
 * A list of all EplPlatformData structs.
 *
 * This is here for cleanup handling.
 */
static struct glvnd_list platform_data_list = { &platform_data_list, &platform_data_list };
static pthread_mutex_t platform_data_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static __attribute__((constructor)) void LibraryInit(void)
{
    eplInitRecursiveMutex(&display_list_mutex);
}

static __attribute__((destructor)) void LibraryFini(void)
{
    pthread_mutex_destroy(&display_list_mutex);
}

EPL_REFCOUNT_DEFINE_TYPE_FUNCS(EplPlatformData, eplPlatformData, refcount, free);
EPL_REFCOUNT_DEFINE_TYPE_FUNCS(EplInternalDisplay, eplInternalDisplay, refcount, free);
EPL_REFCOUNT_DEFINE_TYPE_FUNCS(EplDisplay, eplDisplay, refcount, DestroyDisplay);

EplPlatformData *eplPlatformBaseAllocate(int major, int minor,
        const EGLExtDriver *driver, EGLExtPlatform *extplatform,
        EGLenum platform_enum, const EplImplFuncs *impl,
        size_t platform_priv_size)
{
    EplPlatformData *platform = NULL;
    const char *str;

    // Assert that all of the required implementation functions are provided.
    assert(impl->QueryString != NULL);
    assert(impl->GetPlatformDisplay != NULL);
    assert(impl->CleanupDisplay != NULL);
    assert(impl->InitializeDisplay != NULL);
    assert(impl->TerminateDisplay != NULL);
    assert(impl->DestroySurface != NULL);

    // SwapBuffers is only required if the platform supports windows.
    assert(impl->CreateWindowSurface == NULL || impl->SwapBuffers != NULL);

    // HACK: EGL_EXTERNAL_PLATFORM_VERSION_CHECK is backwards and doesn't work.
    if (extplatform == NULL || !EGL_EXTERNAL_PLATFORM_VERSION_CMP(major, minor,
                EGL_EXTERNAL_PLATFORM_VERSION_MAJOR, EGL_EXTERNAL_PLATFORM_VERSION_MINOR))
    {
        return NULL;
    }

    platform = calloc(1, sizeof(EplPlatformData) + platform_priv_size);
    if (platform == NULL)
    {
        return NULL;
    }
    eplRefCountInit(&platform->refcount);

    platform->impl = impl;
    platform->platform_enum = platform_enum;
    if (platform_priv_size > 0)
    {
        platform->priv = (EplImplPlatform *) (platform + 1);
    }

    glvnd_list_init(&platform->entry);
    glvnd_list_init(&platform->internal_display_list);

    platform->callbacks.getProcAddress = driver->getProcAddress;
    platform->callbacks.setError = driver->setError;
    platform->callbacks.debugMessage = driver->debugMessage;

    platform->egl.QueryString = driver->getProcAddress("eglQueryString");
    platform->egl.GetPlatformDisplay = driver->getProcAddress("eglGetPlatformDisplay");
    platform->egl.Initialize = driver->getProcAddress("eglInitialize");
    platform->egl.Terminate = driver->getProcAddress("eglTerminate");
    platform->egl.GetError = driver->getProcAddress("eglGetError");
    platform->egl.CreatePbufferSurface = driver->getProcAddress("eglCreatePbufferSurface");
    platform->egl.DestroySurface = driver->getProcAddress("eglDestroySurface");
    platform->egl.SwapBuffers = driver->getProcAddress("eglSwapBuffers");
    platform->egl.GetCurrentDisplay = driver->getProcAddress("eglGetCurrentDisplay");
    platform->egl.GetCurrentSurface = driver->getProcAddress("eglGetCurrentSurface");
    platform->egl.GetCurrentContext = driver->getProcAddress("eglGetCurrentContext");
    platform->egl.MakeCurrent = driver->getProcAddress("eglMakeCurrent");
    platform->egl.WaitGL = driver->getProcAddress("eglWaitGL");
    platform->egl.WaitNative = driver->getProcAddress("eglWaitNative");
    platform->egl.SwapInterval = driver->getProcAddress("eglSwapInterval");
    platform->egl.WaitClient = driver->getProcAddress("eglWaitClient");
    platform->egl.ChooseConfig = driver->getProcAddress("eglChooseConfig");
    platform->egl.GetConfigAttrib = driver->getProcAddress("eglGetConfigAttrib");
    platform->egl.GetConfigs = driver->getProcAddress("eglGetConfigs");
    platform->egl.QuerySurface = driver->getProcAddress("eglQuerySurface");
    platform->egl.QueryDeviceAttribEXT = driver->getProcAddress("eglQueryDeviceAttribEXT");
    platform->egl.QueryDeviceStringEXT = driver->getProcAddress("eglQueryDeviceStringEXT");
    platform->egl.QueryDevicesEXT = driver->getProcAddress("eglQueryDevicesEXT");
    platform->egl.QueryDisplayAttribEXT = driver->getProcAddress("eglQueryDisplayAttribEXT");

    // Optional functions.
    platform->egl.SwapBuffersWithDamage = driver->getProcAddress("eglSwapBuffersWithDamageKHR");
    if (platform->egl.SwapBuffersWithDamage == NULL)
    {
        platform->egl.SwapBuffersWithDamage = driver->getProcAddress("eglSwapBuffersWithDamageEXT");
    }
    platform->egl.CreateStreamProducerSurfaceKHR = driver->getProcAddress("CreateStreamProducerSurfaceKHR");

    if (platform->egl.QueryString == NULL
            || platform->egl.QueryString == NULL
            || platform->egl.GetPlatformDisplay == NULL
            || platform->egl.Initialize == NULL
            || platform->egl.Terminate == NULL
            || platform->egl.GetError == NULL
            || platform->egl.CreatePbufferSurface == NULL
            || platform->egl.DestroySurface == NULL
            || platform->egl.SwapBuffers == NULL
            || platform->egl.GetCurrentDisplay == NULL
            || platform->egl.GetCurrentSurface == NULL
            || platform->egl.GetCurrentContext == NULL
            || platform->egl.MakeCurrent == NULL
            || platform->egl.ChooseConfig == NULL
            || platform->egl.GetConfigAttrib == NULL
            || platform->egl.GetConfigs == NULL
            || platform->egl.QuerySurface == NULL
            || platform->egl.QueryDeviceAttribEXT == NULL
            || platform->egl.QueryDeviceStringEXT == NULL
            || platform->egl.QueryDevicesEXT == NULL
            || platform->egl.QueryDisplayAttribEXT == NULL)
    {
        eplPlatformDataUnref(platform);
        return NULL;
    }

    // Check for any extensions that we care about.
    str = platform->egl.QueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    platform->extensions.display_reference = eplFindExtension("EGL_KHR_display_reference", str);

    extplatform->version.major = EGL_EXTERNAL_PLATFORM_VERSION_MAJOR;
    extplatform->version.minor = EGL_EXTERNAL_PLATFORM_VERSION_MINOR;
    extplatform->version.micro = 0;
    extplatform->platform = platform_enum;

    extplatform->exports.unloadEGLExternalPlatform = eplUnloadExternalPlatformExport;
    extplatform->exports.getHookAddress = eplGetHookAddressExport;
    extplatform->exports.isValidNativeDisplay = eplIsValidNativeDisplayExport;
    extplatform->exports.getPlatformDisplay = eplGetPlatformDisplayExport;
    extplatform->exports.queryString = eplQueryStringExport;
    extplatform->exports.getInternalHandle = eplGetInternalHandleExport;
    //extplatform->exports.getObjectLabel = eplGetObjectLabelExport;
    extplatform->data = platform;

    return platform;
}

void eplPlatformBaseInitFinish(EplPlatformData *plat)
{
    pthread_mutex_lock(&platform_data_list_mutex);
    glvnd_list_add(&plat->entry, &platform_data_list);
    pthread_mutex_unlock(&platform_data_list_mutex);
}

void eplPlatformBaseInitFail(EplPlatformData *plat)
{
    assert(glvnd_list_is_empty(&plat->entry));
    eplPlatformDataUnref(plat);
}

static EGLBoolean eplUnloadExternalPlatformExport(void *platformData)
{
    EplPlatformData *platform = platformData;
    EplDisplay *pdpy, *pdpyTmp;

    if (platform == NULL)
    {
        return EGL_TRUE;
    }

    pthread_mutex_lock(&platform_data_list_mutex);
    glvnd_list_del(&platform->entry);
    pthread_mutex_unlock(&platform_data_list_mutex);

    platform->destroyed = EGL_TRUE;

    pthread_mutex_lock(&display_list_mutex);
    glvnd_list_for_each_entry_safe(pdpy, pdpyTmp, &display_list, entry)
    {
        if (pdpy->platform != platform)
        {
            continue;
        }

        // Remove the display from the list and decrement its refcount.
        glvnd_list_del(&pdpy->entry);

        pthread_rwlock_wrlock(&pdpy->init_lock);
        TerminateDisplay(pdpy);
        pthread_rwlock_unlock(&pdpy->init_lock);

        // Note that if some other thread is still holding a reference to this
        // display, then it might get leaked.
        // TODO: Should we just unconditionally free the display here? If
        // another thread is in the middle of a function call, then it's going
        // to crash anyway.
        eplDisplayUnref(pdpy);
    }
    pthread_mutex_unlock(&display_list_mutex);

    // Free the internal display list. Note that the driver will already have
    // terminated all of the internal eglDisplays.
    while (!glvnd_list_is_empty(&platform->internal_display_list))
    {
        EplInternalDisplay *idpy = glvnd_list_first_entry(&platform->internal_display_list, EplInternalDisplay, entry);
        glvnd_list_del(&idpy->entry);
        idpy->edpy = EGL_NO_DISPLAY;
        eplInternalDisplayUnref(idpy);
    }

    if (platform->impl->CleanupPlatform != NULL)
    {
        platform->impl->CleanupPlatform(platform);
    }
    eplPlatformDataUnref(platform);
    return EGL_FALSE;
}

/**
 * This looks up and locks an EGLDisplay, but it does not check whether the
 * display is initialized.
 */
static EplDisplay *eplLookupDisplay(EGLDisplay edpy)
{
    EplDisplay *pdpy = NULL;
    EplDisplay *node = NULL;

    if (edpy == EGL_NO_DISPLAY)
    {
        return NULL;
    }

    pthread_mutex_lock(&display_list_mutex);
    glvnd_list_for_each_entry(node, &display_list, entry)
    {
        if (node->external_display == edpy)
        {
            pdpy = node;
            break;
        }
    }

    if (pdpy == NULL)
    {
        pthread_mutex_unlock(&display_list_mutex);
        return NULL;
    }

    eplDisplayRef(pdpy);

    pthread_mutex_unlock(&display_list_mutex);

    return pdpy;
}

EplDisplay *eplDisplayAcquire(EGLDisplay edpy)
{
    EplDisplay *pdpy = eplLookupDisplay(edpy);

    if (pdpy == NULL)
    {
        return NULL;
    }

    pthread_rwlock_rdlock(&pdpy->init_lock);

    if (!pdpy->initialized)
    {
        eplSetError(pdpy->platform, EGL_NOT_INITIALIZED, "EGLDisplay %p is not initialized", edpy);
        pthread_rwlock_unlock(&pdpy->init_lock);
        eplDisplayUnref(pdpy);
        return NULL;
    }

    return pdpy;
}

void eplGetCurrentSurface(EGLDisplay *ret_edpy, EGLSurface *ret_esurf)
{
    EplPlatformData *plat;
    EGLDisplay edpy = EGL_NO_DISPLAY;
    EGLSurface esurf = EGL_NO_SURFACE;

    /*
     * In practice, loadEGLExternalPlatform is only ever going to get called
     * once (and if it was more than once, it would still be from the same
     * driver), so we'll only have one eglGetCurrentDisplay implementation.
     *
     * But, if there ever is the chance of getting loaded by more than one
     * driver, only one of them could have a current context, so the following
     * code would still work.
     */
    pthread_mutex_lock(&platform_data_list_mutex);
    glvnd_list_for_each_entry(plat, &platform_data_list, entry)
    {
        edpy = plat->egl.GetCurrentDisplay();
        if (edpy != EGL_NO_DISPLAY)
        {
            esurf = plat->egl.GetCurrentSurface(EGL_DRAW);
            break;
        }
    }
    pthread_mutex_unlock(&platform_data_list_mutex);

    if (ret_edpy != NULL)
    {
        *ret_edpy = edpy;
    }
    if (ret_esurf != NULL)
    {
        *ret_esurf = esurf;
    }
}

static void DestroyAllSurfaces(EplDisplay *pdpy)
{
    pthread_rwlock_wrlock(&pdpy->surface_list_lock);

    while (!glvnd_list_is_empty(&pdpy->surface_list))
    {
        EplSurface *psurf = glvnd_list_first_entry(&pdpy->surface_list, EplSurface, entry);

        DeleteSurfaceCommon(pdpy, psurf);
    }

    pthread_rwlock_unlock(&pdpy->surface_list_lock);
}

static void DestroyDisplay(EplDisplay *pdpy)
{
    assert(pdpy != NULL);
    assert(pdpy->refcount.refcount == 0);

    DestroyAllSurfaces(pdpy);

    pdpy->platform->impl->CleanupDisplay(pdpy);
    pthread_rwlock_destroy(&pdpy->init_lock);
    pthread_rwlock_destroy(&pdpy->surface_list_lock);

    eplPlatformDataUnref(pdpy->platform);
    free(pdpy);
}

void eplDisplayRelease(EplDisplay *pdpy)
{
    if (pdpy == NULL)
    {
        return;
    }

    pthread_rwlock_unlock(&pdpy->init_lock);

    eplDisplayUnref(pdpy);
}

EplInternalDisplay *eplLookupInternalDisplay(EplPlatformData *platform, EGLDisplay handle)
{
    EplInternalDisplay *found = NULL;
    EplInternalDisplay *node = NULL;

    pthread_mutex_lock(&platform->internal_display_list_mutex);
    glvnd_list_for_each_entry(node, &platform->internal_display_list, entry)
    {
        if (node->edpy == handle)
        {
            found = node;
            break;
        }
    }

    if (found == NULL)
    {
        found = calloc(1, sizeof(EplInternalDisplay));
        if (found != NULL)
        {
            eplRefCountInit(&found->refcount);
            found->edpy = handle;
            found->init_count = 0;
            glvnd_list_add(&found->entry, &platform->internal_display_list);
        }
    }
    pthread_mutex_unlock(&platform->internal_display_list_mutex);

    return found;
}

EplInternalDisplay *eplGetDeviceInternalDisplay(EplPlatformData *platform, EGLDeviceEXT dev)
{
    static const EGLAttrib TRACK_REFS_ATTRIBS[] = { EGL_TRACK_REFERENCES_KHR, EGL_TRUE, EGL_NONE };
    EGLDisplay handle = EGL_NO_DISPLAY;
    const EGLAttrib *attribs = NULL;

    if (platform->extensions.display_reference)
    {
        attribs = TRACK_REFS_ATTRIBS;
    }

    handle = platform->egl.GetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, dev, attribs);
    if (handle == EGL_NO_DISPLAY)
    {
        return EGL_NO_DISPLAY;
    }

    return eplLookupInternalDisplay(platform, handle);
}

EGLBoolean eplInitializeInternalDisplay(EplPlatformData *platform,
        EplInternalDisplay *idpy, EGLint *major, EGLint *minor)
{
    if (idpy == NULL)
    {
        return EGL_FALSE;
    }

    pthread_mutex_lock(&platform->internal_display_list_mutex);
    if (idpy->init_count == 0)
    {
        if (!platform->egl.Initialize(idpy->edpy, &idpy->major, &idpy->minor))
        {
            pthread_mutex_unlock(&platform->internal_display_list_mutex);
            return EGL_FALSE;
        }
    }
    idpy->init_count++;

    if (major != NULL)
    {
        *major = idpy->major;
    }
    if (minor != NULL)
    {
        *minor = idpy->minor;
    }

    pthread_mutex_unlock(&platform->internal_display_list_mutex);
    return EGL_TRUE;
}

EGLBoolean eplTerminateInternalDisplay(EplPlatformData *platform, EplInternalDisplay *idpy)
{
    if (idpy == NULL)
    {
        return EGL_FALSE;
    }

    pthread_mutex_lock(&platform->internal_display_list_mutex);
    if (idpy->init_count > 0)
    {
        if (idpy->init_count == 1)
        {
            if (!platform->egl.Terminate(idpy->edpy))
            {
                pthread_mutex_unlock(&platform->internal_display_list_mutex);
                return EGL_FALSE;
            }
        }
        idpy->init_count--;
    }
    pthread_mutex_unlock(&platform->internal_display_list_mutex);

    return EGL_TRUE;
}

const struct glvnd_list *eplDisplayLockSurfaceList(EplDisplay *pdpy)
{
    pthread_rwlock_rdlock(&pdpy->surface_list_lock);

    return &pdpy->surface_list;
}

void eplDisplayUnlockSurfaceList(EplDisplay *pdpy)
{
    pthread_rwlock_unlock(&pdpy->surface_list_lock);
}

EplSurface *eplSurfaceListLookup(const struct glvnd_list *surface_list, EGLSurface esurf)
{
    EplSurface *psurf;

    if (esurf == EGL_NO_SURFACE)
    {
        return NULL;
    }

    glvnd_list_for_each_entry(psurf, surface_list, entry)
    {
        if (psurf->external_surface == esurf)
        {
            return psurf;
        }
    }

    return NULL;
}

EGLBoolean eplHookDisplaySurface(EGLDisplay edpy, EGLSurface esurf,
        EplDisplay **ret_pdpy, EplSurface **ret_psurf)
{
    EplDisplay *pdpy = eplDisplayAcquire(edpy);
    const struct glvnd_list *surface_list = NULL;
    EplSurface *psurf = NULL;

    *ret_pdpy = NULL;
    *ret_psurf = NULL;

    if (pdpy == NULL)
    {
        return EGL_FALSE;
    }

    if (esurf == EGL_NO_SURFACE)
    {
        eplSetError(pdpy->platform, EGL_BAD_SURFACE, "EGLSurface handle is EGL_NO_SURFACE");
        eplDisplayRelease(pdpy);
        return EGL_FALSE;
    }

    surface_list = eplDisplayLockSurfaceList(pdpy);
    psurf = eplSurfaceListLookup(surface_list, esurf);
    if (psurf == NULL)
    {
        eplDisplayUnlockSurfaceList(pdpy);
    }
    *ret_pdpy = pdpy;
    *ret_psurf = psurf;
    return EGL_TRUE;
}

void eplHookDisplaySurfaceEnd(EplDisplay *pdpy, const EplSurface *psurf)
{
    if (psurf != NULL)
    {
        eplDisplayUnlockSurfaceList(pdpy);
    }
    eplDisplayRelease(pdpy);
}

static EGLDisplay eplGetPlatformDisplayExport(void *platformData,
        EGLenum platform, void *nativeDisplay, const EGLAttrib* attribs)
{
    EplPlatformData *plat = platformData;
    EGLAttrib *remainingAttribs = NULL;
    EplDisplay *pdpy = NULL;
    EplDisplay *node;
    EGLDisplay ret = EGL_NO_DISPLAY;
    int attribCount = 0;
    int attribIndex = 0;
    int i;

    EGLBoolean track_references = EGL_FALSE;

    if (platform != plat->platform_enum)
    {
        return EGL_NO_DISPLAY;
    }

    // First, make a copy of the attribs array, and pull out any attributes
    // that we care about.
    if (attribs != NULL)
    {
        while (attribs[attribCount] != EGL_NONE)
        {
            attribCount += 2;
        }
    }
    remainingAttribs = alloca((attribCount + 1) * sizeof(EGLAttrib));
    for (i=0; i<attribCount; i += 2)
    {
        if (attribs[i] == EGL_TRACK_REFERENCES_KHR)
        {
            track_references = (attribs[i + 1] != 0);
        }
        else
        {
            if (plat->impl->IsSameDisplay == NULL)
            {
                /*
                 * If we don't have an IsSameDisplay function, then the
                 * platform doesn't support any additional attributes.
                 */
                eplSetError(plat, EGL_BAD_ATTRIBUTE, "Unsupported attribute 0x%04llx",
                        (unsigned long long) attribs[i]);
                return EGL_NO_DISPLAY;
            }
            remainingAttribs[attribIndex++] = attribs[i];
            remainingAttribs[attribIndex++] = attribs[i + 1];
        }
    }
    remainingAttribs[attribIndex] = EGL_NONE;

    pthread_mutex_lock(&display_list_mutex);
    glvnd_list_for_each_entry(node, &display_list, entry)
    {
        if (node->track_references != track_references)
        {
            continue;
        }
        if (node->native_display != nativeDisplay)
        {
            continue;
        }

        if (plat->impl->IsSameDisplay != NULL)
        {
            if (!plat->impl->IsSameDisplay(plat, node, platform, nativeDisplay, remainingAttribs))
            {
                continue;
            }
        }

        // At this point, either IsSameDisplay returned true, or we don't have
        // any additional attributes beyond what the platform base code handles.

        pdpy = node;
        break;
    }

    if (pdpy != NULL)
    {
        // We found a matching display, so return it
        ret = pdpy->external_display;
        goto done;
    }

    pdpy = calloc(1, sizeof(EplDisplay));
    if (pdpy == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Out of memory");
        goto done;
    }

    if (pthread_rwlock_init(&pdpy->init_lock, NULL) != 0)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Failed to create internal locks");
        free(pdpy);
        goto done;
    }

    if (pthread_rwlock_init(&pdpy->surface_list_lock, NULL) != 0)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Failed to create internal locks");
        pthread_rwlock_destroy(&pdpy->init_lock);
        free(pdpy);
        goto done;
    }

    pdpy->platform = eplPlatformDataRef(plat);
    pdpy->platform_enum = platform;
    pdpy->external_display = (EGLDisplay) pdpy;
    pdpy->track_references = track_references;
    pdpy->native_display = nativeDisplay;
    glvnd_list_init(&pdpy->surface_list);
    glvnd_list_init(&pdpy->entry);

    if (!plat->impl->GetPlatformDisplay(plat, pdpy, nativeDisplay, remainingAttribs, &display_list))
    {
        pthread_rwlock_destroy(&pdpy->init_lock);
        pthread_rwlock_destroy(&pdpy->surface_list_lock);
        eplPlatformDataUnref(pdpy->platform);
        free(pdpy);
        ret = EGL_NO_DISPLAY;
        goto done;
    }

    eplRefCountInit(&pdpy->refcount);
    glvnd_list_add(&pdpy->entry, &display_list);
    ret = pdpy->external_display;

done:
    pthread_mutex_unlock(&display_list_mutex);
    return ret;
}

static EGLBoolean HookInitialize(EGLDisplay edpy, EGLint *major, EGLint *minor)
{
    EplDisplay *pdpy = eplLookupDisplay(edpy);

    if (pdpy == NULL)
    {
        return EGL_FALSE;
    }

    pthread_rwlock_wrlock(&pdpy->init_lock);

    if (!pdpy->initialized)
    {
        pdpy->major = 1;
        pdpy->minor = 5;
        if (!pdpy->platform->impl->InitializeDisplay(pdpy->platform, pdpy, &pdpy->major, &pdpy->minor))
        {
            pthread_rwlock_unlock(&pdpy->init_lock);
            eplDisplayUnref(pdpy);
            return EGL_FALSE;
        }
        pdpy->initialized = EGL_TRUE;
        pdpy->init_count = 1;
    }
    else
    {
        if (pdpy->track_references)
        {
            pdpy->init_count++;
        }
        else
        {
            pdpy->init_count = 1;
        }
    }

    if (major != NULL)
    {
        *major = pdpy->major;
    }
    if (minor != NULL)
    {
        *minor = pdpy->minor;
    }

    pthread_rwlock_unlock(&pdpy->init_lock);
    eplDisplayUnref(pdpy);
    return EGL_TRUE;
}

static void TerminateDisplay(EplDisplay *pdpy)
{
    pdpy->init_count = 0;
    if (pdpy->initialized)
    {
        pdpy->initialized = EGL_FALSE;

        DestroyAllSurfaces(pdpy);
        pdpy->platform->impl->TerminateDisplay(pdpy->platform, pdpy);
    }
}

static void CheckTerminateDisplay(EplDisplay *pdpy)
{
    if (pdpy->initialized)
    {
        if (pdpy->init_count == 0)
        {
            TerminateDisplay(pdpy);
        }
    }
}

static EGLBoolean HookTerminate(EGLDisplay edpy)
{
    EplDisplay *pdpy = eplLookupDisplay(edpy);

    if (pdpy == NULL)
    {
        return EGL_FALSE;
    }

    pthread_rwlock_wrlock(&pdpy->init_lock);

    if (pdpy->init_count > 0)
    {
        pdpy->init_count--;
        CheckTerminateDisplay(pdpy);
    }

    pthread_rwlock_unlock(&pdpy->init_lock);
    eplDisplayUnref(pdpy);
    return EGL_TRUE;
}

static EGLAttrib *ConvertIntAttribs(const EGLint *int_attribs)
{
    EGLAttrib *attribs = NULL;
    int count = 0;
    int i;

    if (int_attribs == NULL)
    {
        return NULL;
    }

    for (count = 0; int_attribs[count] != EGL_NONE; count += 2)
    { }

    attribs = malloc((count + 1) * sizeof(EGLAttrib));
    if (attribs == NULL)
    {
        return NULL;
    }

    for (i=0; i<count; i++)
    {
        attribs[i] = int_attribs[i];
    }
    attribs[count] = EGL_NONE;
    return attribs;
}

static EplSurface *AllocBaseSurface(EplPlatformData *plat)
{
    EplSurface *psurf = calloc(1, sizeof(EplSurface));
    if (psurf == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Out of memory");
        return NULL;
    }

    return psurf;
}

static void FreeBaseSurface(EplSurface *psurf)
{
    if (psurf != NULL)
    {
        free(psurf);
    }
}

static EGLSurface CommonCreateSurface(EplDisplay *pdpy,
        EGLConfig config, void *native_handle, const EGLAttrib *attrib_list,
        EplSurfaceType type, EGLBoolean create_platform)
{
    EplSurface *psurf = NULL;
    EGLSurface ret = EGL_NO_SURFACE;

    psurf = AllocBaseSurface(pdpy->platform);
    if (psurf == NULL)
    {
        return EGL_NO_SURFACE;
    }

    psurf->type = type;

    pthread_rwlock_wrlock(&pdpy->surface_list_lock);

    if (type == EPL_SURFACE_TYPE_WINDOW)
    {
        if (pdpy->platform->impl->CreateWindowSurface != NULL)
        {
            psurf->internal_surface = pdpy->platform->impl->CreateWindowSurface(pdpy->platform,
                    pdpy, psurf, config, native_handle, attrib_list, create_platform,
                    &pdpy->surface_list);
        }
        else
        {
            eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Window surfaces are not supported");
        }
    }
    else if (type == EPL_SURFACE_TYPE_PIXMAP)
    {
        if (pdpy->platform->impl->CreatePixmapSurface != NULL)
        {
            psurf->internal_surface = pdpy->platform->impl->CreatePixmapSurface(pdpy->platform,
                    pdpy, psurf, config, native_handle, attrib_list, create_platform,
                    &pdpy->surface_list);
        }
        else
        {
            eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Pixmap surfaces are not supported");
        }
    }
    else
    {
        assert(!"Can't happen: Invalid surface type");
        psurf->internal_surface = EGL_NO_SURFACE;
    }

    if (psurf->internal_surface != EGL_NO_SURFACE)
    {
        psurf->external_surface = (EGLSurface) psurf;
        ret = psurf->external_surface;
        glvnd_list_add(&psurf->entry, &pdpy->surface_list);
    }
    else
    {
        FreeBaseSurface(psurf);
    }

    pthread_rwlock_unlock(&pdpy->surface_list_lock);
    return ret;
}

static EGLSurface HookCreatePlatformWindowSurface(EGLDisplay edpy, EGLConfig config, void *native_window, const EGLAttrib *attrib_list)
{
    EplDisplay *pdpy = eplDisplayAcquire(edpy);
    EGLSurface esurf;

    if (pdpy == NULL)
    {
        return EGL_NO_SURFACE;
    }

    esurf = CommonCreateSurface(edpy, config, native_window, attrib_list, EPL_SURFACE_TYPE_WINDOW, EGL_TRUE);
    eplDisplayRelease(pdpy);
    return esurf;
}

static EGLSurface HookCreateWindowSurface(EGLDisplay edpy, EGLConfig config, EGLNativeWindowType win, const EGLint *attrib_list)
{
    EplDisplay *pdpy = eplDisplayAcquire(edpy);
    EGLAttrib *attribs = NULL;
    EGLSurface esurf;

    if (pdpy == NULL)
    {
        return EGL_NO_SURFACE;
    }

    attribs = ConvertIntAttribs(attrib_list);
    if (attribs == NULL && attrib_list != NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Out of memory");
        eplDisplayRelease(pdpy);
        return EGL_NO_DISPLAY;
    }

    esurf = CommonCreateSurface(edpy, config, (void *) win, attribs, EPL_SURFACE_TYPE_WINDOW, EGL_FALSE);
    free(attribs);
    eplDisplayRelease(pdpy);
    return esurf;
}

static EGLSurface HookCreatePlatformPixmapSurface(EGLDisplay edpy, EGLConfig config, void *native_pixmap, const EGLAttrib *attrib_list)
{
    EplDisplay *pdpy = eplDisplayAcquire(edpy);
    EGLSurface esurf;

    if (pdpy == NULL)
    {
        return EGL_NO_SURFACE;
    }

    esurf = CommonCreateSurface(edpy, config, native_pixmap, attrib_list, EPL_SURFACE_TYPE_PIXMAP, EGL_TRUE);
    eplDisplayRelease(pdpy);
    return esurf;
}

static EGLSurface HookCreatePixmapSurface(EGLDisplay edpy, EGLConfig config, EGLNativePixmapType win, const EGLint *attrib_list)
{
    EplDisplay *pdpy = eplDisplayAcquire(edpy);
    EGLAttrib *attribs = NULL;
    EGLSurface esurf;

    if (pdpy == NULL)
    {
        return EGL_NO_SURFACE;
    }

    attribs = ConvertIntAttribs(attrib_list);
    if (attribs == NULL && attrib_list != NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Out of memory");
        eplDisplayRelease(pdpy);
        return EGL_NO_DISPLAY;
    }

    esurf = CommonCreateSurface(edpy, config, (void *) win, attribs, EPL_SURFACE_TYPE_PIXMAP, EGL_FALSE);
    free(attribs);
    eplDisplayRelease(pdpy);
    return esurf;
}

static EGLSurface HookCreatePbufferSurface(EGLDisplay edpy, EGLConfig config, const EGLint *attrib_list)
{
    EplDisplay *pdpy = NULL;
    EGLSurface esurf;

    pdpy = eplDisplayAcquire(edpy);
    if (pdpy == NULL)
    {
        return EGL_NO_SURFACE;
    }

    // The driver requires that we provide a hook for eglCreatePbufferSurface,
    // but we can just pass it through to the driver.
    esurf = pdpy->platform->egl.CreatePbufferSurface(pdpy->internal_display, config, attrib_list);

    eplDisplayRelease(pdpy);

    return esurf;
}

static void DeleteSurfaceCommon(EplDisplay *pdpy, EplSurface *psurf)
{
    glvnd_list_del(&psurf->entry);

    pdpy->platform->impl->DestroySurface(pdpy, psurf, &pdpy->surface_list);

    FreeBaseSurface(psurf);
}

static EGLBoolean HookDestroySurface(EGLDisplay edpy, EGLSurface esurf)
{
    EplDisplay *pdpy;
    EplSurface *elem;
    EplSurface *psurf = NULL;
    EGLBoolean ret = EGL_FALSE;

    pdpy = eplDisplayAcquire(edpy);
    if (pdpy == NULL)
    {
        return EGL_FALSE;
    }

    pthread_rwlock_wrlock(&pdpy->surface_list_lock);

    glvnd_list_for_each_entry(elem, &pdpy->surface_list, entry)
    {
        if (elem->external_surface == esurf)
        {
            psurf = elem;
            break;
        }
    }

    if (psurf != NULL)
    {
        DeleteSurfaceCommon(pdpy, psurf);
        ret = EGL_TRUE;
    }
    else
    {
        // This EGLSurface doesn't belong to the platform library, so just pass
        // it on to the driver.
        ret = pdpy->platform->egl.DestroySurface(pdpy->internal_display, esurf);
    }

    pthread_rwlock_unlock(&pdpy->surface_list_lock);
    eplDisplayRelease(pdpy);
    return ret;
}

static EGLBoolean HookSwapBuffersWithDamage(EGLDisplay edpy, EGLSurface esurf, const EGLint *rects, EGLint n_rects)
{
    EplDisplay *pdpy;
    EplSurface *psurf;
    EGLBoolean ret = EGL_FALSE;

    if (!eplHookDisplaySurface(edpy, esurf, &pdpy, &psurf))
    {
        return EGL_FALSE;
    }

    if (pdpy->platform->egl.GetCurrentDisplay() != edpy)
    {
        eplSetError(pdpy->platform, EGL_BAD_SURFACE, "EGLDisplay %p is not current", edpy);
        eplHookDisplaySurfaceEnd(pdpy, psurf);
        return EGL_FALSE;
    }

    if (psurf != NULL)
    {
        if (psurf->type != EPL_SURFACE_TYPE_WINDOW)
        {
            eplSetError(pdpy->platform, EGL_BAD_SURFACE, "EGLSurface %p is not a window", esurf);
            ret = EGL_FALSE;
        }
        else if (pdpy->platform->egl.GetCurrentSurface(EGL_DRAW) != esurf)
        {
            eplSetError(pdpy->platform, EGL_BAD_SURFACE, "EGLSurface %p is not current", esurf);
            ret = EGL_FALSE;
        }
        else
        {
            ret = pdpy->platform->impl->SwapBuffers(pdpy->platform, pdpy, psurf, rects, n_rects);
            if (ret)
            {
                psurf->setDamageRegionCalled = EGL_FALSE;
                psurf->bufferAgeCalled = EGL_FALSE;
            }
        }

        eplHookDisplaySurfaceEnd(pdpy, psurf);
    }
    else
    {
        // If we don't recognize this EGLSurface, then it might be a pbuffer or
        // stream, so just pass it through to the driver.
        EGLDisplay internal = pdpy->internal_display;
        PFNEGLSWAPBUFFERSWITHDAMAGEKHRPROC SwapBuffersWithDamage = pdpy->platform->egl.SwapBuffersWithDamage;
        PFNEGLSWAPBUFFERSPROC SwapBuffers = pdpy->platform->egl.SwapBuffers;

        // Release the display before calling into the driver, so that we don't
        // sit on the lock for a (potentially long) SwapBuffers operation.
        eplHookDisplaySurfaceEnd(pdpy, psurf);

        if (SwapBuffersWithDamage != NULL && rects != NULL && n_rects > 0)
        {
            ret = SwapBuffersWithDamage(internal, esurf, rects, n_rects);
        }
        else
        {
            ret = SwapBuffers(internal, esurf);
        }
        return ret;
    }

    return ret;
}

static EGLBoolean HookSwapBuffers(EGLDisplay edpy, EGLSurface esurf)
{
    return HookSwapBuffersWithDamage(edpy, esurf, NULL, 0);
}

static EGLBoolean HookWaitGL(void)
{
    EGLDisplay edpy = EGL_NO_DISPLAY;
    EGLDisplay esurf = EGL_NO_SURFACE;
    EplDisplay *pdpy = NULL;
    EplSurface *psurf = NULL;
    EGLBoolean ret = EGL_FALSE;

    eplGetCurrentSurface(&edpy, &esurf);

    if (!eplHookDisplaySurface(edpy, esurf, &pdpy, &psurf))
    {
        return EGL_FALSE;
    }

    assert(pdpy->platform->impl->WaitGL != NULL);
    if (pdpy->platform->impl->WaitGL != NULL)
    {
        ret = pdpy->platform->impl->WaitGL(pdpy, psurf);
    }
    else
    {
        // This shouldn't happen, because we only provide an eglWaitGL hook if
        // we have an implementation. But, if we wanted to handle this case,
        // then we could just forward the call through to the driver.
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Internal error: eglWaitGL hook should not be called");
    }

    eplHookDisplaySurfaceEnd(pdpy, psurf);
    return ret;
}

static EGLBoolean HookWaitNative(void)
{
    EGLDisplay edpy = EGL_NO_DISPLAY;
    EGLDisplay esurf = EGL_NO_SURFACE;
    EplDisplay *pdpy = NULL;
    EplSurface *psurf = NULL;
    EGLBoolean ret = EGL_FALSE;

    eplGetCurrentSurface(&edpy, &esurf);
    if (!eplHookDisplaySurface(edpy, esurf, &pdpy, &psurf))
    {
        return EGL_FALSE;
    }

    assert(pdpy->platform->impl->WaitNative != NULL);
    if (pdpy->platform->impl->WaitNative != NULL)
    {
        ret = pdpy->platform->impl->WaitNative(pdpy, psurf);
    }
    else
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Internal error: eglWaitNative hook should not be called");
    }

    eplHookDisplaySurfaceEnd(pdpy, psurf);
    return ret;
}

static EGLBoolean HookQueryDisplayAttrib(EGLDisplay edpy, EGLint attribute, EGLAttrib *value)
{
    EplDisplay *pdpy = eplDisplayAcquire(edpy);
    EGLBoolean ret = EGL_FALSE;

    if (pdpy == NULL)
    {
        return EGL_FALSE;
    }
    if (value == NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_PARAMETER, "value pointer must not be NULL");
        eplDisplayRelease(pdpy);
        return EGL_FALSE;
    }

    if (attribute == EGL_TRACK_REFERENCES_KHR)
    {
        *value = (EGLAttrib) pdpy->track_references;
        ret = EGL_TRUE;
    }
    else if (pdpy->platform->impl->QueryDisplayAttrib != NULL)
    {
        ret = pdpy->platform->impl->QueryDisplayAttrib(pdpy, attribute, value);
    }
    else
    {
        ret = pdpy->platform->egl.QueryDisplayAttribEXT(pdpy->internal_display, attribute, value);
    }

    eplDisplayRelease(pdpy);
    return ret;
}

static EGLBoolean HookSwapInterval(EGLDisplay edpy, EGLint interval)
{
    EplDisplay *pdpy = NULL;
    EGLBoolean ret = EGL_FALSE;
    EGLDisplay internal_edpy = EGL_NO_DISPLAY;
    PFNEGLSWAPINTERVALPROC SwapInterval = NULL;

    pdpy = eplDisplayAcquire(edpy);
    if (pdpy == NULL)
    {
        return EGL_FALSE;
    }

    if (pdpy->platform->egl.GetCurrentDisplay() == edpy)
    {
        EGLSurface esurf = pdpy->platform->egl.GetCurrentSurface(EGL_DRAW);
        const struct glvnd_list *surface_list = eplDisplayLockSurfaceList(pdpy);
        EplSurface *psurf = eplSurfaceListLookup(surface_list, esurf);
        if (psurf != NULL)
        {
            if (pdpy->platform->impl->SwapInterval != NULL)
            {
                ret = pdpy->platform->impl->SwapInterval(pdpy, psurf, interval);
            }
            else
            {
                // This should never happen: If we don't have a SwapInterval
                // implementation, then we shouldn't have provided an
                // eglSwapInterval hook to the driver.
                assert(!"Can't happen -- no SwapInterval implementation");
                ret = EGL_TRUE;
            }
        }
        else
        {
            // If we don't recognize he current EGLSurface, then we'll just
            // pass the call through to the driver after we unlock everything.
            internal_edpy = pdpy->internal_display;
            SwapInterval = pdpy->platform->egl.SwapInterval;
        }
        eplDisplayUnlockSurfaceList(pdpy);
    }
    else
    {
        eplSetError(pdpy->platform, EGL_BAD_SURFACE, "EGLDisplay %p is not current", edpy);
    }

    if (SwapInterval != NULL)
    {
        ret = SwapInterval(internal_edpy, interval);
    }

    eplDisplayRelease(pdpy);
    return ret;
}

static EGLBoolean HookQuerySurface(EGLDisplay edpy, EGLSurface esurf, EGLint attribute, EGLint *value)
{
    EplDisplay *pdpy;
    EplSurface *psurf;
    EGLBoolean ret = EGL_FALSE;

    if (!eplHookDisplaySurface(edpy, esurf, &pdpy, &psurf))
    {
        return EGL_FALSE;
    }

    if (value == NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_PARAMETER, "value pointer must not be NULL");
        goto done;
    }

    if (psurf != NULL)
    {
        if (attribute == EGL_BUFFER_AGE_KHR)
        {
            if (pdpy->platform->egl.GetCurrentSurface(EGL_DRAW) != esurf)
            {
                eplSetError(pdpy->platform, EGL_BAD_SURFACE, "EGLSurface %p is not current", esurf);
                goto done;
            }

            // Note: This is where we'll call into the platform-specific code
            // once we add a query function to EplImplFuncs. Until then,
            // returning zero is valid (if not very useful).
            *value = 0;

            psurf->bufferAgeCalled = EGL_TRUE;
            ret = EGL_TRUE;
        }
        else
        {
            // If it's not an attribute that we recognize, then pass the query
            // through to the driver.
            ret = pdpy->platform->egl.QuerySurface(pdpy->internal_display,
                    psurf->internal_surface, attribute, value);
        }
    }
    else
    {
        // If it's not an EGLSurface that we recognize, then pass the query
        // through to the driver.
        ret = pdpy->platform->egl.QuerySurface(pdpy->internal_display,
                esurf, attribute, value);
    }

done:
    eplHookDisplaySurfaceEnd(pdpy, psurf);
    return ret;
}

static EGLBoolean HookSetDamageRegion(EGLDisplay edpy, EGLSurface esurf, EGLint *rects, EGLint n_rects)
{
    EplDisplay *pdpy;
    EplSurface *psurf;
    EGLBoolean ret = EGL_FALSE;

    if (!eplHookDisplaySurface(edpy, esurf, &pdpy, &psurf))
    {
        return EGL_FALSE;
    }

    if (psurf != NULL)
    {
        if (psurf->type != EPL_SURFACE_TYPE_WINDOW)
        {
            eplSetError(pdpy->platform, EGL_BAD_MATCH, "EGLSurface %p is not a postable surface", esurf);
            goto done;
        }
        if (pdpy->platform->egl.GetCurrentSurface(EGL_DRAW) != esurf)
        {
            eplSetError(pdpy->platform, EGL_BAD_MATCH, "EGLSurface %p is not current", esurf);
            goto done;
        }
        if (!psurf->bufferAgeCalled)
        {
            eplSetError(pdpy->platform, EGL_BAD_ACCESS,
                    "EGL_BUFFER_AGE_KHR must be queried before calling eglSetDamageRegionKHR");
            goto done;
        }
        if (psurf->setDamageRegionCalled)
        {
            eplSetError(pdpy->platform, EGL_BAD_ACCESS,
                    "eglSetDamageRegionKHR has already been called this frame");
            goto done;
        }

        /*
         * Here's where we'd optionally call into the platform-specific code.
         * Since eglSetDamageRegionKHR is just a hint, it's also valid to just
         * ignore it.
         */

        psurf->setDamageRegionCalled = EGL_TRUE;
        ret = EGL_TRUE;
    }
    else if (pdpy->platform->egl.SetDamageRegionKHR != NULL)
    {
        // We don't recgonize this EGLSurface, so pass it through to the driver.
        ret = pdpy->platform->egl.SetDamageRegionKHR(pdpy->internal_display, esurf, rects, n_rects);
    }
    else
    {
        // If the driver doesn't support eglSetDamageRegionKHR, then just
        // ignore it and return success.
        ret = EGL_TRUE;
    }

done:
    eplHookDisplaySurfaceEnd(pdpy, psurf);
    return ret;
}

static const EplHookFunc BASE_HOOK_FUNCTIONS[] =
{
    { "eglCreatePbufferSurface", HookCreatePbufferSurface },
    { "eglCreatePixmapSurface", HookCreatePixmapSurface },
    { "eglCreatePlatformPixmapSurface", HookCreatePlatformPixmapSurface },
    { "eglCreatePlatformWindowSurface", HookCreatePlatformWindowSurface },
    { "eglCreateWindowSurface", HookCreateWindowSurface },
    { "eglDestroySurface", HookDestroySurface },
    { "eglInitialize", HookInitialize },
    { "eglQueryDisplayAttribEXT", HookQueryDisplayAttrib },
    { "eglQueryDisplayAttribKHR", HookQueryDisplayAttrib },
    { "eglQueryDisplayAttribNV", HookQueryDisplayAttrib },
    { "eglQuerySurface", HookQuerySurface },
    { "eglSetDamageRegionKHR", HookSetDamageRegion },
    { "eglSwapBuffers", HookSwapBuffers },
    { "eglSwapBuffersWithDamageEXT", HookSwapBuffersWithDamage },
    { "eglSwapBuffersWithDamageKHR", HookSwapBuffersWithDamage },
    { "eglTerminate", HookTerminate },
};
static const size_t BASE_HOOK_FUNCTION_COUNT = sizeof(BASE_HOOK_FUNCTIONS) / sizeof(BASE_HOOK_FUNCTIONS[0]);

void *eplGetHookAddressExport(void *platformData, const char *name)
{
    EplPlatformData *plat = platformData;
    void *func = NULL;

    func = eplFindHookFunction(BASE_HOOK_FUNCTIONS, BASE_HOOK_FUNCTION_COUNT, name);
    if (func != NULL)
    {
        return func;
    }

    if (plat->impl->GetHookFunction != NULL)
    {
        func = plat->impl->GetHookFunction(plat, name);
        if (func != NULL)
        {
            return func;
        }
    }

    if (plat->impl->WaitGL != NULL && strcmp(name, "eglWaitGL") == 0)
    {
        return HookWaitGL;
    }
    if (plat->impl->WaitNative != NULL && strcmp(name, "eglWaitNative") == 0)
    {
        return HookWaitNative;
    }
    if (plat->impl->SwapInterval != NULL && strcmp(name, "eglSwapInterval") == 0)
    {
        return HookSwapInterval;
    }
    return NULL;
}

static EGLBoolean eplIsValidNativeDisplayExport(void *platformData, void *nativeDisplay)
{
    EplPlatformData *plat = platformData;
    if (plat->impl->IsValidNativeDisplay != NULL)
    {
        return plat->impl->IsValidNativeDisplay(platformData, nativeDisplay);
    }
    else
    {
        return EGL_FALSE;
    }
}

static const char *eplQueryStringExport(void *platformData, EGLDisplay edpy, EGLExtPlatformString name)
{
    EplPlatformData *plat = platformData;
    EplDisplay *pdpy = NULL;
    const char *str;

    if (edpy != EGL_NO_DISPLAY)
    {
        pdpy = eplDisplayAcquire(edpy);
        if (pdpy == NULL)
        {
            return NULL;
        }
    }

    str = plat->impl->QueryString(plat, pdpy, name);
    if (pdpy != NULL)
    {
        eplDisplayRelease(pdpy);
    }
    return str;
}

static void *eplGetInternalHandleExport(EGLDisplay edpy, EGLenum type, void *handle)
{
    void *ret = NULL;

    if (type == EGL_OBJECT_DISPLAY_KHR)
    {
        EplDisplay *pdpy = eplLookupDisplay(handle);

        if (pdpy != NULL)
        {
            pthread_rwlock_rdlock(&pdpy->init_lock);
            if (pdpy->initialized)
            {
                ret = pdpy->internal_display;
            }
            pthread_rwlock_unlock(&pdpy->init_lock);
            eplDisplayUnref(pdpy);
        }
    }
    else
    {
        EplDisplay *pdpy = eplLookupDisplay(edpy);
        if (pdpy != NULL)
        {
            pthread_rwlock_rdlock(&pdpy->init_lock);

            if (pdpy->initialized)
            {
                if (type == EGL_OBJECT_SURFACE_KHR)
                {
                    const struct glvnd_list *surface_list = eplDisplayLockSurfaceList(pdpy);
                    EplSurface *psurf = eplSurfaceListLookup(surface_list, (EGLSurface) handle);
                    if (psurf != NULL)
                    {
                        ret = psurf->internal_surface;
                    }
                    else
                    {
                        /*
                         * Assume that if we don't recognize the handle, then it's
                         * a pbuffer or stream surface, and so the driver should
                         * just pass it through. If the handle is invalid, then the
                         * driver should then set the appropriate error code on its
                         * own.
                         */
                        ret = handle;
                    }
                    eplDisplayUnlockSurfaceList(pdpy);
                }
            }
            pthread_rwlock_unlock(&pdpy->init_lock);
            eplDisplayUnref(pdpy);
        }
    }

    return ret;
}

void eplSetError(EplPlatformData *platform, EGLint error, const char *fmt, ...)
{
    EGLint messageType;
    const char *message = NULL;
    char buf[1024];
    va_list args;

    if (error == EGL_BAD_ALLOC)
    {
        messageType = EGL_DEBUG_MSG_CRITICAL_KHR;
    }
    else
    {
        messageType = EGL_DEBUG_MSG_ERROR_KHR;
    }

    if (fmt != NULL)
    {
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        message = buf;
    }
    platform->callbacks.setError(error, messageType, message);
}

EGLBoolean eplSwitchCurrentSurface(EplPlatformData *platform, EplDisplay *pdpy,
        EGLSurface old_surface, EGLSurface new_surface)
{
    EGLSurface new_draw = EGL_NO_SURFACE;
    EGLSurface new_read = EGL_NO_SURFACE;

    if (platform->egl.GetCurrentDisplay() != pdpy->internal_display)
    {
        // The display isn't current, so the surface can't be.
        return EGL_TRUE;
    }

    new_draw = platform->egl.GetCurrentSurface(EGL_DRAW);
    new_read = platform->egl.GetCurrentSurface(EGL_READ);
    if (new_draw != old_surface && new_read != old_surface)
    {
        return EGL_TRUE;
    }

    if (new_draw == old_surface)
    {
        new_draw = new_surface;
    }
    if (new_read == old_surface)
    {
        new_read = new_surface;
    }

    return platform->egl.MakeCurrent(platform->egl.GetCurrentDisplay(),
            new_draw, new_read,
            platform->egl.GetCurrentContext());
}

EGLDeviceEXT *eplGetAllDevices(EplPlatformData *platform, EGLint *ret_count)
{
    EGLint count = 0;
    EGLDeviceEXT *devices = NULL;

    if (!platform->egl.QueryDevicesEXT(0, NULL, &count))
    {
        return NULL;
    }

    devices = malloc((count + 1) * sizeof(EGLDeviceEXT));
    if (devices == NULL)
    {
        eplSetError(platform, EGL_BAD_ALLOC, "Out of memory");
        return NULL;
    }

    if (count > 0)
    {
        if (!platform->egl.QueryDevicesEXT(count, devices, &count))
        {
            free(devices);
            return NULL;
        }
    }
    devices[count] = EGL_NO_DEVICE_EXT;
    if (ret_count != NULL)
    {
        *ret_count = count;
    }
    return devices;
}

struct glvnd_list *eplLockDisplayList(void)
{
    pthread_mutex_lock(&display_list_mutex);
    return &display_list;
}

void eplUnlockDisplayList(void)
{
    pthread_mutex_unlock(&display_list_mutex);
}
