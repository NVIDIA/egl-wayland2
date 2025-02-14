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

#ifndef WAYLAND_TIMELINE_H
#define WAYLAND_TIMELINE_H

/**
 * \file
 *
 * Functions for dealing with timeline sync objects.
 */

#include <EGL/egl.h>

#include "wayland-platform.h"
#include "wayland-display.h"

typedef struct
{
    uint32_t handle;
    uint64_t point;
    struct wp_linux_drm_syncobj_timeline_v1 *wtimeline;
} WlTimeline;

/**
 * Creates and initializes a timeline sync object.
 *
 * This will create a timeline object, and share it with the server using
 * linux-explicit-synchronization-unstable-v1.
 */
EGLBoolean eplWlTimelineInit(WlDisplayInstance *inst, WlTimeline *timeline);
void eplWlTimelineDestroy(WlDisplayInstance *inst, WlTimeline *timeline);

/**
 * Attaches a sync FD to the next timeline point.
 *
 * On a successful return, \c timeline->point will be the timeline point where
 * the sync FD was attached.
 */
EGLBoolean eplWlTimelineAttachSyncFD(WlDisplayInstance *inst, WlTimeline *timeline, int syncfd);

/**
 * Extracts a sync FD from the current timeline point.
 */
int eplWlTimelinePointToSyncFD(WlDisplayInstance *inst, WlTimeline *timeline);

#endif // WAYLAND_TIMELINE_H
