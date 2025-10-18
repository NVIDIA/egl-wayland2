# SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

project('nvidia-egl-wayland2', 'c',
  version : '1.0.1',
  default_options : ['c_std=gnu99'],
)

dep_libdrm = dependency('libdrm')
dep_threads = dependency('threads')
dep_eglexternal = dependency('eglexternalplatform', version : ['>=1.2', '<2'])
dep_egl = dependency('egl', version : ['>=1.5', '<2']).partial_dependency(includes : true, compile_args : true)
inc_base = include_directories('src/base')

cc = meson.get_compiler('c')
if cc.compiles('typeof(int *);', name : 'typeof')
  add_project_arguments('-DHAVE_TYPEOF', language : ['c'])
endif

subdir('src/base')
subdir('src/wayland')

