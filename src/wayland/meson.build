# SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

dep_gbm = dependency('gbm')
dep_wayland_client = dependency('wayland-client')
dep_dl = meson.get_compiler('c').find_library('dl', required : false)

wl_protos = dependency('wayland-protocols', version: '>= 1.34')
wl_protos_dir = wl_protos.get_variable('pkgdatadir')
wl_drm_xml = 'wayland-drm.xml'
wl_dmabuf_xml = join_paths(wl_protos_dir, 'unstable', 'linux-dmabuf', 'linux-dmabuf-unstable-v1.xml')
wl_drm_syncobj_xml = join_paths(wl_protos_dir, 'staging', 'linux-drm-syncobj', 'linux-drm-syncobj-v1.xml')

wl_scanner = dependency('wayland-scanner', native: true)
prog_scanner = find_program(wl_scanner.get_variable('wayland_scanner'))

client_header = generator(prog_scanner,
    output : '@BASENAME@-client-protocol.h',
    arguments : ['client-header', '@INPUT@', '@OUTPUT@']
)
if wl_scanner.version().version_compare('>= 1.14.91')
    code_arg = 'private-code'
else
    code_arg = 'code'
endif
code = generator(prog_scanner,
    output : '@BASENAME@-protocol.c',
    arguments : [code_arg, '@INPUT@', '@OUTPUT@']
)

generated_files = [
  client_header.process(wl_drm_xml),
  code.process(wl_drm_xml),

  client_header.process(wl_dmabuf_xml),
  code.process(wl_dmabuf_xml),

  client_header.process(wl_drm_syncobj_xml),
  code.process(wl_drm_syncobj_xml),
]

wayland_platform = shared_library('nvidia-egl-wayland2',
  [
    'wayland-platform.c',
    'wayland-display.c',
    'wayland-dmabuf.c',
    'wayland-fbconfig.c',
    'wayland-timeline.c',
    generated_files,
  ],
  include_directories: [ inc_base ],
  c_args : ['-D_GNU_SOURCE'],
    dependencies: [
    dep_eglexternal,
    dep_libdrm,
    dep_threads,
    dep_gbm,
    dep_wayland_client,
    dep_dl,
  ],
  link_with: [ platform_base ],
  version : meson.project_version(),
  gnu_symbol_visibility: 'hidden',
  install: true)

install_data('09_nvidia_wayland2.json',
  install_dir: '@0@/egl/egl_external_platform.d'.format(get_option('datadir')))
