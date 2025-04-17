# Wayland EGL External Platform library, Version 2

> [!warning]
> This library is still in development.

This is a new implementation of the EGL External Platform Library for Wayland
(`EGL_KHR_platform_wayland`), using the NVIDIA driver's new platform surface
interface, which simplifies a lot of the library and improves window resizing.

See [Implementation Notes](#implementation-notes) for more details.

## Building and Installing

This library depends on:
- libgbm, version 21.2.0
- libdrm, version 2.4.99
- wayland-protocols, version 1.38 (https://gitlab.freedesktop.org/wayland/wayland-protocols)
- libwayland-client and wayland-scanner (https://gitlab.freedesktop.org/wayland/wayland)
- EGL headers (https://www.khronos.org/registry/EGL)
- EGL External Platform interface, version 1.2 (https://github.com/NVIDIA/eglexternalplatform)

In addition, this library depends on an interface in the NVIDIA driver which is
supported in 560 and later series drivers.

For full functionality, it also needs a compositor that supports the
linux-drm-syncobj-v1 protocol for explicit sync. Without explicit sync, you may
get reduced performance and out-of-order frames.

To build and install, use Meson:
```sh
meson builddir
ninja -C builddir
ninja -C builddir install
```

This library can be installed alongside the previous egl-wayland implementation
(https://github.com/NVIDIA/egl-wayland). The new library has a higher selection
priority by default, so if both are present, then a 560 or later driver will
select the new library, and an older driver will fall back to the old library.

## Compositor Requirements

This library requires a compositor that supports the dma-buf protocols. That
means either of:
- linux-dmabuf-v1 version >= 4, AND libdrm version >= 2.4.109
- linux-dmabuf-v1 version 3, AND wl-drm

For synchronization, it will use the explicit sync protocols if available,
which requires:
- linux-drm-syncobj-v1
- libdrm version 2.4.99

Without implicit sync, the library can still run, but you may get reduced
performance and out-of-order frames.

If available, the library will use the presentation-time, fifo-v1, and
commit-timing-v1 protocols for vsync and frame throttling. Without those, a
swap interval greater than 1 won't work, and eglSwapBuffers may block
indefinately if the window is not visible (e.g., there's another window in
front of it).

## Notes for Application Developers

This library follows the same general protocol rules as the Wayland WSI for
Vulkan:
- The `wl_surface.attach` and `wl_surface.commit` requests, along with any
  other related requests (explicit sync points, damage areas, frame
  throttling) are sent in `eglSwapBuffers`.
- The library uses its own `wl_event_queue`s for any proxies that it creates,
  and handles all events internally.

To avoid problems, if an EGLSurface for a `wl_surface` exists, then
applications should not directly send `wl_surface.commit` requests for that
surface. If you need to change any other double-buffered state for a surface,
then send those requests before calling `eglSwapBuffers`.

In particular, an application MUST NOT send a `wl_surface.commit` request for
a surface concurrently with an `eglSwapBuffers` call. Due to the lack of
orthogonality between surface changes and the lack of any synchronization
across multiple requests, surface commits in Wayland are not thread-safe.

Even with a single thread, due to limitations in the fifo-v1 and
presentation-time protocols, sending a `wl_surface.commit` request at all
outside of `eglSwapBuffers` will break frame throttling, and may result in
discarded frames.

## Known Issues and Workarounds

### Explicit Sync Compatibility

Some applications can run into problems with the explicit sync protocols,
especially if they try to send `wl_surface.commit` requests from multiple
threads.

If that happens, you can disable explicit sync by setting an environment
variable `__NV_DISABLE_EXPLICIT_SYNC=1`.

## Implementation Notes

This implementation uses a new driver interface (added in the 560 series
drivers) that allows it to allocate a dma-buf and then attach it as a color
buffer for an EGLSurface. Conceptually, it's similar to an FBO.

See [driver-platform-surface.h](src/wayland/driver-platform-surface.h) for more
details on this interface.

That provides more direct control over buffer selection and reuse than the
previous EGLStream-based implementation.

In particular, this library should handle window resizing better, especially in
cases such as calling `wl_egl_window_resize` for a window that isn't the
current EGLSurface.

Because EGLStreams are not resizable, resizing a window in the old library
requires creating an new EGLSurface internally, and then calling eglMakeCurrent
to switch to it.

If the application calls `wl_egl_window_resize` when the window isn't current,
then the library can't safely apply the new size until the next eglSwapBuffers
call, so you end up with an entire frame with the old size. While that's
technically allowed by the EGL spec, it means that this sequence doesn't work
the way you'd expect:

```c
window = wl_egl_window_create(wl_surface, 100, 100);
EGLSurface surf = eglCreateWindowSurface(dpy, config, window, NULL);
wl_egl_window_resize(window, 200, 200, 0, 0);
eglMakeCurrent(dpy, surf, surf, ctx);
glViewport(200, 200);
/*
 * Draw stuff, but the rendering is distorted because OpenGL's viewport
 * transformation is for 200x200, but the window is still 100x100.
 */
eglSwapBuffers(dpy, surf); // Displays a 100x100 window
```

With the new platform surface interface, the driver will call into the platform
library at certain points to let the platform deal with a window resize. As a
result, an application can call `wl_egl_window_resize` from any thread, and
the resize will be applied as soon as it calls `glViewport`.
