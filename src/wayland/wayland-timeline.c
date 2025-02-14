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

#include "wayland-timeline.h"

#include <string.h>
#include <unistd.h>
#include <assert.h>

EGLBoolean eplWlTimelineInit(WlDisplayInstance *inst, WlTimeline *timeline)
{
    int fd = -1;
    int ret;
    EGLBoolean success = EGL_FALSE;

    memset(timeline, 0, sizeof(*timeline));

    if (inst->globals.syncobj == NULL)
    {
        assert(inst->globals.syncobj != NULL);
        return EGL_FALSE;
    }
    assert(inst->globals.syncobj != NULL);

    ret = inst->platform->priv->drm.SyncobjCreate(
            gbm_device_get_fd(inst->gbmdev),
            0, &timeline->handle);
    if (ret != 0)
    {
        return EGL_FALSE;
    }

    ret = inst->platform->priv->drm.SyncobjHandleToFD(
            gbm_device_get_fd(inst->gbmdev),
            timeline->handle, &fd);
    if (ret != 0 || fd < 0)
    {
        goto done;
    }

    timeline->wtimeline = wp_linux_drm_syncobj_manager_v1_import_timeline(inst->globals.syncobj, fd);
    if (timeline->wtimeline == NULL)
    {
        goto done;
    }

    success = EGL_TRUE;

done:
    // libwayland-client will duplicate the file descriptor for its request
    // queue, so we need to close our copy here.
    if (fd >= 0)
    {
        close(fd);
    }
    if (!success)
    {
        inst->platform->priv->drm.SyncobjDestroy(
                gbm_device_get_fd(inst->gbmdev),
                timeline->handle);
        memset(timeline, 0, sizeof(*timeline));
    }

    return success;
}

void eplWlTimelineDestroy(WlDisplayInstance *inst, WlTimeline *timeline)
{
    if (timeline->wtimeline != NULL)
    {
        wp_linux_drm_syncobj_timeline_v1_destroy(timeline->wtimeline);

        inst->platform->priv->drm.SyncobjDestroy(
                gbm_device_get_fd(inst->gbmdev),
                timeline->handle);

        timeline->wtimeline = NULL;
        timeline->handle = 0;
        timeline->point = 0;
    }
}

int eplWlTimelinePointToSyncFD(WlDisplayInstance *inst, WlTimeline *timeline)
{
    uint32_t tempobj = 0;
    int syncfd = -1;

    if (inst->platform->priv->drm.SyncobjCreate(
                gbm_device_get_fd(inst->gbmdev),
                0, &tempobj) != 0)
    {
        return EGL_FALSE;
    }

    if (inst->platform->priv->drm.SyncobjTransfer(gbm_device_get_fd(inst->gbmdev),
			      tempobj, 0, timeline->handle, timeline->point, 0) != 0)
    {
        goto done;
    }

    if (inst->platform->priv->drm.SyncobjExportSyncFile(gbm_device_get_fd(inst->gbmdev),
            tempobj, &syncfd) != 0)
    {
        goto done;
    }

done:
    inst->platform->priv->drm.SyncobjDestroy(
            gbm_device_get_fd(inst->gbmdev),
            tempobj);
    return syncfd;
}

EGLBoolean eplWlTimelineAttachSyncFD(WlDisplayInstance *inst, WlTimeline *timeline, int syncfd)
{
    uint32_t tempobj = 0;
    EGLBoolean success = EGL_FALSE;

    assert(syncfd >= 0);

    if (inst->platform->priv->drm.SyncobjCreate(
                gbm_device_get_fd(inst->gbmdev),
                0, &tempobj) != 0)
    {
        // TODO: Issue an EGL error here?
        return EGL_FALSE;
    }

    if (inst->platform->priv->drm.SyncobjImportSyncFile(
                gbm_device_get_fd(inst->gbmdev),
                tempobj, syncfd) != 0)
    {
        goto done;
    }

    if (inst->platform->priv->drm.SyncobjTransfer(
                gbm_device_get_fd(inst->gbmdev),
                timeline->handle, timeline->point + 1,
                tempobj, 0, 0) != 0)
    {
        goto done;
    }

    timeline->point++;
    success = EGL_TRUE;

done:
    inst->platform->priv->drm.SyncobjDestroy(
            gbm_device_get_fd(inst->gbmdev),
            tempobj);
    return success;
}
