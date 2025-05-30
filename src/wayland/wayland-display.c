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

#include "wayland-display.h"

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>

#include "wayland-drm-client-protocol.h"

#include "platform-utils.h"
#include "wayland-fbconfig.h"

// The minimum and maximum versions of each protocol that we support.
static const uint32_t PROTO_DMABUF_VERSION[2] = { 3, 4 };
static const uint32_t PROTO_SYNC_OBJ_VERSION[2] = { 1, 1 };
static const uint32_t PROTO_DRM_VERSION[2] = { 1, 1 };
static const uint32_t PROTO_PRESENTATION_TIME_VERSION[2] = { 1, 2 };
static const uint32_t PROTO_FIFO_VERSION[2] = { 1, 1 };
static const uint32_t PROTO_COMMIT_TIMING_VERSION[2] = { 1, 1 };

typedef struct
{
    uint32_t name;
    uint32_t version;
} WlDisplayGlobalName;

typedef struct
{
    const char *name;
    uint32_t version;
} ProtocolVersionOverride;

/**
 * Holds the object names and versions for the global Wayland protocol objects
 * that we care about.
 */
typedef struct
{
    struct wl_registry *registry;
    ProtocolVersionOverride *version_overrides;
    WlDisplayGlobalName zwp_linux_dmabuf_v1;
    WlDisplayGlobalName wp_linux_drm_syncobj_manager_v1;
    WlDisplayGlobalName wp_presentation;
    WlDisplayGlobalName wp_fifo_manager_v1;
    WlDisplayGlobalName wp_commit_timing_manager_v1;
    WlDisplayGlobalName wl_drm;
} WlDisplayRegistry;

static WlDisplayInstance *eplWlDisplayInstanceCreate(EplDisplay *pdpy, EGLBoolean from_init);
static void eplWlDisplayInstanceFree(WlDisplayInstance *inst);

EPL_REFCOUNT_DEFINE_TYPE_FUNCS(WlDisplayInstance, eplWlDisplayInstance, refcount, eplWlDisplayInstanceFree);

EGLBoolean eplWlIsSameDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint platform,
        void *native_display, const EGLAttrib *attribs)
{
    EGLDeviceEXT device = EGL_NO_DEVICE_EXT;
    if (attribs != NULL)
    {
        int i;
        for (i=0; attribs[i] != EGL_NONE; i += 2)
        {
            if (attribs[i] == EGL_DEVICE_EXT)
            {
                device = (EGLDeviceEXT) attribs[i + 1];
            }
            else
            {
                return EGL_FALSE;
            }
        }
    }

    if (device != pdpy->priv->device_attrib)
    {
        return EGL_FALSE;
    }

    return EGL_TRUE;
}

EGLBoolean eplWlGetPlatformDisplay(EplPlatformData *plat, EplDisplay *pdpy,
        void *native_display, const EGLAttrib *attribs,
        struct glvnd_list *existing_displays)
{
    const char *env;
    WlDisplayInstance *inst;

    pdpy->priv = calloc(1, sizeof(EplImplDisplay));
    if (pdpy->priv == NULL)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Out of memory");
        return EGL_FALSE;
    }

    if (attribs != NULL)
    {
        int i;
        for (i=0; attribs[i] != EGL_NONE; i += 2)
        {
            if (attribs[i] == EGL_DEVICE_EXT)
            {
                pdpy->priv->device_attrib = (EGLDeviceEXT) attribs[i + 1];
            }
            else
            {
                eplSetError(plat, EGL_BAD_ATTRIBUTE, "Invalid attribute 0x%lx", (unsigned long) attribs[i]);
                eplWlCleanupDisplay(pdpy);
                return EGL_FALSE;
            }
        }
    }

    env = getenv("__NV_PRIME_RENDER_OFFLOAD_PROVIDER");
    if (env != NULL)
    {
        pdpy->priv->requested_device = eplWlFindDeviceForNode(plat, env);
        pdpy->priv->enable_alt_device = EGL_TRUE;
    }
    else
    {
        env = getenv("__NV_PRIME_RENDER_OFFLOAD");
        if (env != NULL && atoi(env) != 0)
        {
            pdpy->priv->enable_alt_device = EGL_TRUE;
        }
    }

    if (pdpy->priv->requested_device == EGL_NO_DEVICE_EXT)
    {
        // If the caller specified a device, then make sure it's valid.
        if (pdpy->priv->device_attrib != EGL_NO_DEVICE_EXT)
        {
            EGLint num = 0;
            EGLDeviceEXT *devices = eplGetAllDevices(plat, &num);
            EGLBoolean valid = EGL_FALSE;
            EGLint i;

            if (devices == NULL)
            {
                eplWlCleanupDisplay(pdpy);
                return EGL_FALSE;
            }

            for (i=0; i<num; i++)
            {
                if (devices[i] == pdpy->priv->device_attrib)
                {
                    valid = EGL_TRUE;
                    break;
                }
            }
            free(devices);

            if (valid)
            {
                // The requested device is a valid NVIDIA device, so use it.
                pdpy->priv->requested_device = pdpy->priv->device_attrib;
            }
            else if (pdpy->priv->enable_alt_device)
            {
                // The requested device is not an NVIDIA device, but PRIME is
                // enabled, so we'll pick an NVIDIA device during eglInitialize.
                pdpy->priv->requested_device = EGL_NO_DEVICE_EXT;
            }
            else
            {
                // The requested device is not an NVIDIA device and PRIME is not
                // enabled. Return failure to let another driver handle it.
                eplSetError(plat, EGL_BAD_MATCH, "Unknown or non-NV device handle %p",
                        pdpy->priv->device_attrib);
                eplWlCleanupDisplay(pdpy);
                return EGL_FALSE;
            }
        }
    }

    /*
     * Ideally, we'd wait until eglInitialize to open the connection or do the
     * rest of our compatibility checks, but we have to do that now to check
     * whether we can actually support whichever server we're connecting to.
     */
    inst = eplWlDisplayInstanceCreate(pdpy, EGL_FALSE);
    if (inst == NULL)
    {
        eplWlCleanupDisplay(pdpy);
        return EGL_FALSE;
    }
    eplWlDisplayInstanceUnref(inst);

    return EGL_TRUE;
}

void eplWlCleanupDisplay(EplDisplay *pdpy)
{
    if (pdpy->priv != NULL)
    {
        eplWlDisplayInstanceUnref(pdpy->priv->inst);
        free(pdpy->priv);
        pdpy->priv = NULL;
    }
}

EGLBoolean eplWlInitializeDisplay(EplPlatformData *plat, EplDisplay *pdpy, EGLint *major, EGLint *minor)
{
    assert(pdpy->priv->inst == NULL);

    pdpy->priv->inst = eplWlDisplayInstanceCreate(pdpy, EGL_TRUE);
    if (pdpy->priv->inst == NULL)
    {
        return EGL_FALSE;
    }

    if (major != NULL)
    {
        *major = pdpy->priv->inst->internal_display->major;
    }
    if (minor != NULL)
    {
        *minor = pdpy->priv->inst->internal_display->minor;
    }

    pdpy->internal_display = pdpy->priv->inst->internal_display->edpy;
    return EGL_TRUE;
}

void eplWlTerminateDisplay(EplPlatformData *plat, EplDisplay *pdpy)
{
    assert(pdpy->priv->inst != NULL);
    eplWlDisplayInstanceUnref(pdpy->priv->inst);
    pdpy->priv->inst = NULL;
}

static EGLBoolean CheckRegistryGlobal(WlDisplayGlobalName *obj,
        const char *want_iface, const uint32_t need_version[2],
        uint32_t name, const char *iface, uint32_t version)
{
    if (strcmp(iface, want_iface) == 0)
    {
        if (version >= need_version[0])
        {
            if (version > need_version[1])
            {
                version = need_version[1];
            }
            obj->name = name;
            obj->version = version;
        }
        return EGL_TRUE;
    }
    else
    {
        return EGL_FALSE;
    }
}

static int FindNextStringToken(const char **tok, size_t *len, const char *sep)
{
    // Skip to the end of the current name.
    const char *ptr = *tok + *len;

    // Skip any leading separators.
    while (*ptr != '\x00' && strchr(sep, *ptr) != NULL)
    {
        ptr++;
    }

    // Find the length of the current token.
    *len = 0;
    while (ptr[*len] != '\x00' && strchr(sep, ptr[*len]) == NULL)
    {
        (*len)++;
    }
    *tok = ptr;
    return (*len > 0 ? 1 : 0);
}

static ProtocolVersionOverride *ParseProtocolOverrideString(const char *str)
{
    ProtocolVersionOverride *ret = NULL;
    char *strbuf;
    const char *tok = str;
    size_t len = 0;
    size_t num_tokens = 0;
    size_t total_len = 0;

    if (str == NULL)
    {
        return NULL;
    }

    while (FindNextStringToken(&tok, &len, ","))
    {
        num_tokens++;
        total_len += len + 1;
    }

    if (num_tokens == 0)
    {
        return NULL;
    }

    ret = malloc((num_tokens + 1) * sizeof(ProtocolVersionOverride) + total_len);
    if (ret == NULL)
    {
        return NULL;
    }

    strbuf = (char *) (ret + num_tokens + 1);

    tok = str;
    len = 0;
    num_tokens = 0;
    while (FindNextStringToken(&tok, &len, ","))
    {
        const char *sep = strchr(tok, '=');
        if (sep != NULL)
        {
            size_t namelen = (sep - tok);
            memcpy(strbuf, tok, namelen);
            strbuf[namelen] = '\x00';

            ret[num_tokens].name = strbuf;
            ret[num_tokens].version = atoi(sep + 1);

            num_tokens++;
            strbuf += namelen + 1;
        }
    }
    if (num_tokens == 0)
    {
        free(ret);
        return NULL;
    }

    ret[num_tokens].name = NULL;
    return ret;
}

static void onRegistryGlobal(void *userdata, struct wl_registry *wl_registry,
        uint32_t name, const char *interface, uint32_t version)
{
    WlDisplayRegistry *names = userdata;

    if (names->version_overrides != NULL)
    {
        int i;
        for (i=0; names->version_overrides[i].name != NULL; i++)
        {
            if (strcmp(names->version_overrides[i].name, interface) == 0)
            {
                if (names->version_overrides[i].version <= 0)
                {
                    return;
                }
                else if (names->version_overrides[i].version < version)
                {
                    version = names->version_overrides[i].version;
                }
            }
        }
    }

#define CHECK_INTERFACE(iface, ver_pair) \
        if (CheckRegistryGlobal(&names->iface, #iface, ver_pair, name, interface, version)) return
    CHECK_INTERFACE(zwp_linux_dmabuf_v1, PROTO_DMABUF_VERSION);
    CHECK_INTERFACE(wp_linux_drm_syncobj_manager_v1, PROTO_SYNC_OBJ_VERSION);
    CHECK_INTERFACE(wl_drm, PROTO_DRM_VERSION);
    CHECK_INTERFACE(wp_presentation, PROTO_PRESENTATION_TIME_VERSION);
    CHECK_INTERFACE(wp_fifo_manager_v1, PROTO_FIFO_VERSION);
    CHECK_INTERFACE(wp_commit_timing_manager_v1, PROTO_COMMIT_TIMING_VERSION);
#undef CHECK_INTERFACE
}
static void OnRegistryGlobalRemove(void *data, struct wl_registry *wl_registry, uint32_t name)
{
    // Ignore it. All of the objects that we care about are singletons.
}
static const struct wl_registry_listener REGISTRY_LISTENER = {
    onRegistryGlobal,
    OnRegistryGlobalRemove,
};

static void FreeDisplayRegistry(WlDisplayRegistry *registry)
{
    if (registry != NULL)
    {
        if (registry->registry != NULL)
        {
            wl_registry_destroy(registry->registry);
            registry->registry = NULL;
        }
    }
}

static EGLBoolean GetDisplayRegistry(struct wl_display *wdpy,
        struct wl_event_queue *queue,
        WlDisplayRegistry *names)
{
    struct wl_display *wrapper = NULL;
    EGLBoolean success = EGL_FALSE;

    wrapper = wl_proxy_create_wrapper(wdpy);
    if (wrapper == NULL)
    {
        goto done;
    }

    wl_proxy_set_queue((struct wl_proxy *) wrapper, queue);

    names->version_overrides = ParseProtocolOverrideString(getenv("__NV_WAYLAND_PROTOCOL_VERSIONS"));
    names->registry = wl_display_get_registry(wrapper);
    if (names->registry == NULL)
    {
        goto done;
    }

    if (wl_registry_add_listener(names->registry, &REGISTRY_LISTENER, names) != 0)
    {
        goto done;
    }

    if (wl_display_roundtrip_queue(wdpy, queue) < 0)
    {
        goto done;
    }

    success = EGL_TRUE;

done:
    if (wrapper != NULL)
    {
        wl_proxy_wrapper_destroy(wrapper);
    }
    if (!success)
    {
        FreeDisplayRegistry(names);
    }
    if (names->version_overrides != NULL)
    {
        free(names->version_overrides);
        names->version_overrides = NULL;
    }
    return success;
}

/**
 * Binds a global Wayland object, using a specific wl_event_queue.
 */
static void *BindGlobalObject(struct wl_registry *registry,
        uint32_t name,
        const struct wl_interface *interface,
        uint32_t version,
        struct wl_event_queue *queue)
{
    struct wl_registry *wrapper = wl_proxy_create_wrapper(registry);
    void *proxy = NULL;
    if (wrapper == NULL)
    {
        return NULL;
    }
    wl_proxy_set_queue((struct wl_proxy *) wrapper, queue);
    proxy = wl_registry_bind(wrapper, name, interface, version);
    wl_proxy_wrapper_destroy(wrapper);
    return proxy;
}

void on_wl_drm_device(void *data, struct wl_drm *wl_drm, const char *name)
{
    char **ptr = data;
    free(*ptr);
    *ptr = strdup(name);
}
void on_wl_drm_format(void *data, struct wl_drm *wl_drm, uint32_t format) { }
void on_wl_drm_authenticated(void *data, struct wl_drm *wl_drm) { }
void on_wl_drm_capabilities(void *data, struct wl_drm *wl_drm, uint32_t value) { }
static const struct wl_drm_listener INIT_WL_DRM_LISTENER =
{
    on_wl_drm_device,
    on_wl_drm_format,
    on_wl_drm_authenticated,
    on_wl_drm_capabilities,
};

static char *GetServerDrmNode(struct wl_display *wdpy, const WlDisplayRegistry *names)
{
    struct wl_event_queue *queue = NULL;
    struct wl_drm *drm = NULL;
    char *node = NULL;

    if (names->wl_drm.name != 0)
    {
        queue = wl_display_create_queue(wdpy);
        if (queue == NULL)
        {
            goto done;
        }

        drm = BindGlobalObject(names->registry, names->wl_drm.name, &wl_drm_interface, 1, queue);
        if (drm == NULL)
        {
            goto done;
        }

        wl_drm_add_listener(drm, &INIT_WL_DRM_LISTENER, &node);
        wl_display_roundtrip_queue(wdpy, queue);
    }

done:
    if (drm != NULL)
    {
        wl_drm_destroy(drm);
    }
    if (queue != NULL)
    {
        wl_event_queue_destroy(queue);
    }

    return node;
}

/**
 * Opens a DRM device node, and looks up the corresponding EGLDeviceEXT handle
 * if it's an NVIDIA device.
 *
 * \param plat The platform data.
 * \param devId The device to open and check.
 * \param node An optional device node path. This is used if libdrm is too old
 *      to support drmGetDeviceFromDevId.
 * \param from_init True if this is being called from eglInitialize.
 * \param[out] ret_egldev Returns the EGLDeviceEXT, or EGL_NO_DEVICE_EXT if
 *      it's not an NVIDIA device.
 * \return A file descriptor for the device, or -1 on failure.
 */
static int OpenDrmDevice(EplPlatformData *plat,
        dev_t devId,
        const char *node,
        EGLBoolean from_init,
        EGLDeviceEXT *ret_egldev)
{
    drmDevice *drmdev = NULL;
    int isNV = -1;
    EGLDeviceEXT edev = EGL_NO_DEVICE_EXT;
    int fd = -1;

    if (plat->priv->drm.GetDeviceFromDevId != NULL)
    {
        if (plat->priv->drm.GetDeviceFromDevId(devId, 0, &drmdev) != 0 || drmdev == NULL)
        {
            if (from_init)
            {
                eplSetError(plat, EGL_BAD_ALLOC, "Failed to get DRM device information");
            }
            drmdev = NULL;
        }
    }

    if (drmdev == NULL)
    {
        if (node == NULL)
        {
            if (from_init)
            {
                eplSetError(plat, EGL_BAD_ALLOC, "Didn't get device node from server");
            }
            goto done;
        }

        // Either drmGetDeviceFromDevId failed, or it's not available. In
        // either case, if we have a path from wl_drm, then try using that
        // instead.
        fd = open(node, O_RDWR);
        if (fd < 0)
        {
            if (from_init)
            {
                eplSetError(plat, EGL_BAD_ALLOC, "Can't open device node %s", node);
            }
            goto done;
        }

        if (drmGetDevice(fd, &drmdev) != 0 || drmdev == NULL)
        {
            if (from_init)
            {
                eplSetError(plat, EGL_BAD_ALLOC, "Failed to get DRM device information");
            }
            goto done;
        }
    }
    assert(drmdev != NULL);

    if (drmdev->bustype == DRM_BUS_PCI)
    {
        // If this is a PCI device, then we can just check the vendor ID to
        // know if it's an NVIDIA device or not.
        isNV = (drmdev->deviceinfo.pci->vendor_id == 0x10de);
    }

    // If we didn't open a file descriptor above, then do so now.
    if (fd < 0 && drmdev->available_nodes & (1 << DRM_NODE_RENDER) && drmdev->nodes[DRM_NODE_RENDER] != NULL)
    {
        fd = open(drmdev->nodes[DRM_NODE_RENDER], O_RDWR);
    }
    if (fd < 0 && drmdev->available_nodes & (1 << DRM_NODE_PRIMARY) && drmdev->nodes[DRM_NODE_PRIMARY] != NULL)
    {
        fd = open(drmdev->nodes[DRM_NODE_PRIMARY], O_RDWR);
    }
    if (fd < 0)
    {
        eplSetError(plat, EGL_BAD_ALLOC, "Can't open DRM node for device");
        goto done;
    }

    if (isNV < 0)
    {
        // If we couldn't determine from the PCI info whether this is an NVIDIA
        // device, then use drmGetVersion.
        drmVersion *version = drmGetVersion(fd);
        isNV = 0;
        if (version != NULL)
        {
            if (version->name != NULL)
            {
                if (strcmp(version->name, "nvidia-drm") == 0
                        || strcmp(version->name, "tegra-udrm") == 0
                        || strcmp(version->name, "tegra") == 0)
                {
                    isNV = 1;
                }
            }
            drmFreeVersion(version);
        }
    }

    if (isNV)
    {
        // If this is an NVIDIA device, then find the corresponding
        // EGLDeviceEXT handle.
        if (drmdev->available_nodes & (1 << DRM_NODE_PRIMARY)
                && drmdev->nodes[DRM_NODE_PRIMARY] != NULL)
        {
            edev = eplWlFindDeviceForNode(plat, drmdev->nodes[DRM_NODE_PRIMARY]);
        }
        if (edev == EGL_NO_DEVICE_EXT
                && drmdev->available_nodes & (1 << DRM_NODE_RENDER)
                && drmdev->nodes[DRM_NODE_RENDER] != NULL)
        {
            edev = eplWlFindDeviceForNode(plat, drmdev->nodes[DRM_NODE_RENDER]);
        }
        if (edev == EGL_NO_DEVICE_EXT)
        {
            // This is an NVIDIA device, but the NVIDIA driver can't open it
            // for some reason. Bail out.
            eplSetError(plat, EGL_BAD_ALLOC, "Can't find EGLDeviceEXT handle for device");
            close(fd);
            fd = -1;
            goto done;
        }
    }

done:
    if (drmdev != NULL)
    {
        drmFreeDevice(&drmdev);
    }
    if (ret_egldev != NULL)
    {
        *ret_egldev = edev;
    }
    return fd;
}

static size_t LookupDeviceIds(EplPlatformData *plat, EGLDeviceEXT egldev, dev_t device_ids[2])
{
    const char *extensions = plat->egl.QueryDeviceStringEXT(egldev, EGL_EXTENSIONS);
    size_t count = 0;
    struct stat st;

    if (eplFindExtension("EGL_EXT_device_drm", extensions))
    {
        const char *node = plat->egl.QueryDeviceStringEXT(egldev, EGL_DRM_DEVICE_FILE_EXT);
        if (node == NULL)
        {
            return 0;
        }

        if (stat(node, &st) != 0)
        {
            eplSetError(plat, EGL_BAD_ACCESS, "Can't stat %s: %s", node, strerror(errno));
            return 0;
        }
        device_ids[count++] = st.st_rdev;
    }

    if (eplFindExtension("EGL_EXT_device_drm_render_node", extensions))
    {
        const char *node = plat->egl.QueryDeviceStringEXT(egldev, EGL_DRM_RENDER_NODE_FILE_EXT);
        if (node == NULL)
        {
            return 0;
        }

        if (stat(node, &st) != 0)
        {
            eplSetError(plat, EGL_BAD_ACCESS, "Can't stat %s: %s", node, strerror(errno));
            return 0;
        }
        device_ids[count++] = st.st_rdev;
    }

    if (count == 0)
    {
        // This shouldn't happen: We should always at least suport
        // EGL_EXT_device_drm on every device.
        eplSetError(plat, EGL_BAD_ALLOC, "Driver error: Can't find device node paths");
    }
    return count;
}

static EGLBoolean CheckExplicitSyncSupport(EplPlatformData *plat, int drmfd)
{
    uint64_t cap = 0;
    const char *env;

    if (!plat->priv->timeline_funcs_supported)
    {
        return EGL_FALSE;
    }

    env = getenv("__NV_DISABLE_EXPLICIT_SYNC");
    if (env != NULL && atoi(env) != 0)
    {
        return EGL_FALSE;
    }

    if (plat->priv->drm.GetCap(drmfd, DRM_CAP_SYNCOBJ_TIMELINE, &cap) != 0 || cap == 0)
    {
        return EGL_FALSE;
    }

    return EGL_TRUE;
}

static void on_wp_presentation_clock_id(void *userdata,
        struct wp_presentation *wpp, uint32_t clk_id)
{
    if (userdata != NULL)
    {
        uint32_t *ptr = userdata;
        *ptr = clk_id;
    }
}
static struct wp_presentation_listener PRESENTATION_TIME_LISTENER = { on_wp_presentation_clock_id };

static char *InitExtensionString(const char *internal_ext)
{
    static const char PRESENT_OPAQUE_NAME[] = "EGL_EXT_present_opaque";
    size_t len;
    char *str;

    if (internal_ext == NULL || internal_ext[0] == '\x00')
    {
        return strdup(PRESENT_OPAQUE_NAME);
    }
    if (eplFindExtension(PRESENT_OPAQUE_NAME, internal_ext))
    {
        return strdup(internal_ext);
    }

    len = strlen(internal_ext);
    str = malloc(len + 1 + sizeof(PRESENT_OPAQUE_NAME));
    if (str != NULL)
    {
        memcpy(str, internal_ext, len);
        str[len] = ' ';
        memcpy(str + len + 1, PRESENT_OPAQUE_NAME, sizeof(PRESENT_OPAQUE_NAME));
    }
    return str;
}

WlDisplayInstance *eplWlDisplayInstanceCreate(EplDisplay *pdpy, EGLBoolean from_init)
{
    WlDisplayInstance *inst = NULL;
    WlDisplayRegistry names = {};
    struct wl_event_queue *queue = NULL;
    dev_t mainDevice = 0;
    char *drmNode = NULL;
    int drmFd = -1;
    EGLDeviceEXT serverDevice = EGL_NO_DEVICE_EXT;
    EGLDeviceEXT renderDevice = EGL_NO_DEVICE_EXT;
    const WlDmaBufFormat *fmt;
    EGLBoolean supportsLinear = EGL_FALSE;
    const char *ext = NULL;
    EGLBoolean success = EGL_FALSE;

    inst = calloc(1, sizeof(WlDisplayInstance));
    if (inst == NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Out of memory");
        return NULL;
    }
    eplRefCountInit(&inst->refcount);
    inst->platform = eplPlatformDataRef(pdpy->platform);

    if (pdpy->native_display == NULL)
    {
        inst->own_display = EGL_TRUE;
        inst->wdpy = wl_display_connect(NULL);
        if (inst->wdpy == NULL)
        {
            eplSetError(pdpy->platform, EGL_BAD_ALLOC, "wl_display_connect failed");
            goto done;
        }
    }
    else
    {
        inst->own_display = EGL_FALSE;
        inst->wdpy = pdpy->native_display;
    }

    queue = wl_display_create_queue(inst->wdpy);
    if (queue == NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "wl_display_create_queue failed");
        goto done;
    }

    if (!GetDisplayRegistry(inst->wdpy, queue, &names))
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Failed to get Wayland registry");
        goto done;
    }

    if (names.zwp_linux_dmabuf_v1.name == 0 || names.zwp_linux_dmabuf_v1.version < 3)
    {
        if (from_init)
        {
            eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Server does not support zwp_linux_dmabuf_v1");
        }
        goto done;
    }
    if (names.zwp_linux_dmabuf_v1.version < 4 && names.wl_drm.name == 0)
    {
        /*
         * We need either zwp_linux_dmabuf_v1 version 4, or wl_drm in order to
         * get a device from the server.
         *
         * Note that if the server supports linear, then it would be possible
         * to make this work using our PRIME path. However, it's unlikely that
         * any real-world compositors will support zwp_linux_dmabuf_v1 at
         * exactly version 3, without also supporting wl_drm.
         */
        if (from_init)
        {
            eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Server does not support wl_drm or zwp_linux_dmabuf_v1 version 4");
        }
        goto done;
    }

    inst->globals.dmabuf = BindGlobalObject(names.registry, names.zwp_linux_dmabuf_v1.name,
            &zwp_linux_dmabuf_v1_interface, names.zwp_linux_dmabuf_v1.version, queue);
    if (inst->globals.dmabuf == NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Failed to create zwp_linux_dmabuf_v1 proxy");
        goto done;
    }

    /*
     * Fetch the default set of formats and modifiers from the server.
     *
     * After this, we shouldn't get any more events from the zwp_linux_dmabuf_v1,
     * and if we do, eplWlDmaBufFeedbackGetDefault will have already stubbed it
     * out so that we ignore them.
     *
     * So, we reset the zwp_linux_dmabuf_v1 proxy's queue back to the default,
     * which will allow us to destroy the wl_event_queue before returning.
     */
    inst->default_feedback = eplWlDmaBufFeedbackGetDefault(inst->wdpy, inst->globals.dmabuf, queue, &mainDevice);
    wl_proxy_set_queue((struct wl_proxy *) inst->globals.dmabuf, NULL);
    if (inst->default_feedback == NULL)
    {
        goto done;
    }

    // Get a device node path via wl_drm, if it's available. We'll use that as
    // a fallback if we can't look up the device by a dev_t.
    drmNode = GetServerDrmNode(inst->wdpy, &names);

    drmFd = OpenDrmDevice(pdpy->platform, mainDevice,
            drmNode, from_init, &serverDevice);
    if (drmFd < 0)
    {
        goto done;
    }

    // Check if the server supports linear. If so, then we could support PRIME.
    fmt = eplWlDmaBufFormatFind(inst->default_feedback->formats,
            inst->default_feedback->num_formats, DRM_FORMAT_XRGB8888);
    if (fmt != NULL)
    {
        supportsLinear = eplWlDmaBufFormatSupportsModifier(fmt, DRM_FORMAT_MOD_LINEAR);
    }

    if (pdpy->priv->requested_device != EGL_NO_DEVICE_EXT)
    {
        // The user or app requested a particular device, so try to use it if
        // possible.
        if (pdpy->priv->requested_device == serverDevice || supportsLinear)
        {
            renderDevice = pdpy->priv->requested_device;
        }
    }
    else
    {
        // If the user/app didn't request a specific device, but the server is
        // running on an NVIDIA device, then use the server's device.
        renderDevice = serverDevice;
    }

    if (renderDevice == EGL_NO_DEVICE_EXT && pdpy->priv->enable_alt_device)
    {
        // If we didn't find a device above, but we're allowed to use an
        // alternate, then do so.
        if (serverDevice != EGL_NO_DEVICE_EXT)
        {
            // We can always render to the server's device
            renderDevice = serverDevice;
        }
        else if (supportsLinear)
        {
            EGLint num = 0;
            if (!pdpy->platform->egl.QueryDevicesEXT(1, &renderDevice, &num) || num <= 0)
            {
                renderDevice = EGL_NO_DEVICE_EXT;
            }
        }
    }

    if (renderDevice == EGL_NO_DEVICE_EXT)
    {
        if (from_init)
        {
            eplSetError(pdpy->platform, EGL_BAD_ACCESS, "Display server is not running on an NVIDIA device");
        }
        else if (pdpy->priv->device_attrib != EGL_NO_DEVICE_EXT)
        {
            eplSetError(pdpy->platform, EGL_BAD_MATCH, "GPU offloading from %p is not supported", pdpy->priv->device_attrib);
        }
        goto done;
    }

    if (renderDevice != serverDevice)
    {
        // If we're running on a different device than the server, then we need
        // to open the correct device node for GBM.
        ext = pdpy->platform->egl.QueryDeviceStringEXT(renderDevice, EGL_EXTENSIONS);

        assert(supportsLinear);

        close(drmFd);
        drmFd = -1;

        if (eplFindExtension("EGL_EXT_device_drm_render_node", ext))
        {
            const char *node = pdpy->platform->egl.QueryDeviceStringEXT(renderDevice, EGL_DRM_RENDER_NODE_FILE_EXT);
            if (node != NULL)
            {
                drmFd = open(node, O_RDWR);
            }
        }

        if (drmFd < 0)
        {
            const char *node = pdpy->platform->egl.QueryDeviceStringEXT(renderDevice, EGL_DRM_DEVICE_FILE_EXT);
            if (node == NULL)
            {
                eplSetError(pdpy->platform, EGL_BAD_ACCESS, "Driver error: Can't find device node");
                eplWlDisplayInstanceUnref(inst);
                return NULL;
            }

            drmFd = open(node, O_RDWR);
            if (drmFd < 0)
            {
                eplSetError(pdpy->platform, EGL_BAD_ACCESS, "Can't open device node %s: %s", node, strerror(errno));
                eplWlDisplayInstanceUnref(inst);
                return NULL;
            }
        }

        inst->force_prime = EGL_TRUE;
    }

    // Assume that if the server is running on a non-NVIDIA device, then it
    // supports implicit sync.
    inst->supports_implicit_sync = (serverDevice == EGL_NO_DEVICE_EXT);
    if (inst->supports_implicit_sync)
    {
        // Allow disabling implicit sync. This shouldn't be necessary in
        // practice, but it can be useful for testing.
        const char *env = getenv("__NV_DISABLE_IMPLICIT_SYNC");
        if (env != NULL && atoi(env) != 0)
        {
            inst->supports_implicit_sync = EGL_FALSE;
        }
    }

    inst->gbmdev = gbm_create_device(drmFd);
    if (inst->gbmdev == NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Can't open GBM device");
        goto done;
    }
    drmFd = -1;

    inst->render_device_id_count = LookupDeviceIds(pdpy->platform, renderDevice, inst->render_device_id);
    if (inst->render_device_id_count == 0)
    {
        goto done;
    }

    // Pick an arbitrary device to use as a placeholder for an internal EGLDisplay.
    inst->internal_display = eplInternalDisplayRef(eplGetDeviceInternalDisplay(pdpy->platform, renderDevice));
    if (inst->internal_display == NULL)
    {
        goto done;
    }

    if (!eplInitializeInternalDisplay(pdpy->platform, inst->internal_display, NULL, NULL))
    {
        goto done;
    }

    ext = pdpy->platform->egl.QueryString(inst->internal_display->edpy, EGL_EXTENSIONS);
    inst->supports_EGL_ANDROID_native_fence_sync = eplFindExtension("EGL_ANDROID_native_fence_sync", ext);

    inst->extension_string = InitExtensionString(ext);
    if (inst->extension_string == NULL)
    {
        eplSetError(pdpy->platform, EGL_BAD_ALLOC, "Out of memory");
        goto done;
    }

    if (inst->supports_EGL_ANDROID_native_fence_sync
            && names.wp_linux_drm_syncobj_manager_v1.name != 0
            && CheckExplicitSyncSupport(pdpy->platform, gbm_device_get_fd(inst->gbmdev)))
    {
        inst->globals.syncobj = BindGlobalObject(names.registry,
                names.wp_linux_drm_syncobj_manager_v1.name,
                &wp_linux_drm_syncobj_manager_v1_interface,
                names.wp_linux_drm_syncobj_manager_v1.version,
                NULL);
    }

    if (names.wp_presentation.name != 0
            && names.wp_fifo_manager_v1.name != 0
            && names.wp_commit_timing_manager_v1.name != 0)
    {
        inst->globals.presentation_time = BindGlobalObject(names.registry,
                names.wp_presentation.name, &wp_presentation_interface,
                names.wp_presentation.version, queue);
        if (inst->globals.presentation_time == NULL)
        {
            goto done;
        }
        wp_presentation_add_listener(inst->globals.presentation_time,
                &PRESENTATION_TIME_LISTENER,
                &inst->presentation_time_clock_id);
        wl_display_roundtrip_queue(inst->wdpy, queue);
        // Now that we've got the clock ID, detach it from the event queue
        // so that we can destroy the queue later.
        wl_proxy_set_user_data((struct wl_proxy *) inst->globals.presentation_time, NULL);
        wl_proxy_set_queue((struct wl_proxy *) inst->globals.presentation_time, NULL);

        inst->globals.fifo = BindGlobalObject(names.registry,
                names.wp_fifo_manager_v1.name, &wp_fifo_manager_v1_interface,
                names.wp_fifo_manager_v1.version, NULL);
        if (inst->globals.fifo == NULL)
        {
            goto done;
        }

        inst->globals.commit_timing = BindGlobalObject(names.registry,
                names.wp_commit_timing_manager_v1.name, &wp_commit_timing_manager_v1_interface,
                names.wp_commit_timing_manager_v1.version, NULL);
        if (inst->globals.commit_timing == NULL)
        {
            goto done;
        }
    }

    inst->driver_formats = eplWlGetDriverFormats(pdpy->platform, inst->internal_display->edpy);
    if (inst->driver_formats == NULL)
    {
        goto done;
    }

    inst->configs = eplWlInitConfigList(pdpy->platform, inst->internal_display->edpy,
        inst->default_feedback, inst->driver_formats, EGL_TRUE, inst->force_prime, from_init);
    if (inst->configs == NULL)
    {
        goto done;
    }

    success = EGL_TRUE;

done:
    FreeDisplayRegistry(&names);
    wl_event_queue_destroy(queue);
    free(drmNode);
    if (drmFd >= 0)
    {
        close(drmFd);
    }

    if (!success)
    {
        eplWlDisplayInstanceUnref(inst);
        inst = NULL;
    }

    return inst;
}

static void eplWlDisplayInstanceFree(WlDisplayInstance *inst)
{
    if (inst != NULL)
    {
        if (inst->internal_display != NULL)
        {
            eplTerminateInternalDisplay(inst->platform, inst->internal_display);
            eplInternalDisplayUnref(inst->internal_display);
        }

        if (inst->globals.dmabuf != NULL)
        {
            zwp_linux_dmabuf_v1_destroy(inst->globals.dmabuf);
        }
        if (inst->globals.syncobj != NULL)
        {
            wp_linux_drm_syncobj_manager_v1_destroy(inst->globals.syncobj);
        }

        if (inst->own_display && inst->wdpy != NULL)
        {
            wl_display_disconnect(inst->wdpy);
        }

        if (inst->gbmdev != NULL)
        {
            int fd = gbm_device_get_fd(inst->gbmdev);
            gbm_device_destroy(inst->gbmdev);
            if (fd >= 0)
            {
                close(fd);
            }
        }

        eplWlFormatListFree(inst->default_feedback);
        eplWlFormatListFree(inst->driver_formats);
        eplConfigListFree(inst->configs);

        if (inst->platform != NULL)
        {
            eplPlatformDataUnref(inst->platform);
        }

        free(inst);
    }
}

const char *eplWlHookQueryString(EGLDisplay edpy, EGLint name)
{
    EplDisplay *pdpy = eplDisplayAcquire(edpy);
    const char *str = NULL;

    if (pdpy == NULL)
    {
        return EGL_FALSE;
    }

    if (name == EGL_EXTENSIONS)
    {
        str = pdpy->priv->inst->extension_string;
    }
    if (str == NULL)
    {
        str = pdpy->platform->egl.QueryString(pdpy->internal_display, name);
    }

    eplDisplayRelease(pdpy);
    return str;
}
