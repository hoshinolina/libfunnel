# libfunnel

A library to make creating PipeWire video streams easy, using zero-copy DMA-BUF frame sharing. "Spout2 / Syphon, but for Linux".

## Status

This is still rough around the edges and the API is not considered stable yet.

Features:

- [x] Sending frames
- [ ] Receiving frames
- [ ] Multiple synchronized streams
- [x] Implicit sync
- [x] Hybrid sync (explicit API, implicit stream)
- [ ] Explicit sync
- [x] Async and multiple sync modes (with and without buffering)
- [x] Raw GBM API
- [x] EGL API integration
- [x] Vulkan API integration
- [ ] GLX API integration (if someone really really needs it... ideally apps should switch to X11+EGL)
- [x] Cross-GPU frame sharing for reasonable drivers (untested but it should work?)
- [x] Vulkan: Cross-GPU frame sharing between Nvidia prop driver and other GPUs/drivers (can require special handling by user)
- [ ] GL: Cross-GPU frame sharing between Nvidia prop driver and other GPUs/drivers
- [ ] GL: Automatic optimization for cross-GPU frame sharing (framebuffer optimization/conversion blits)

Note: Due to missing explicit sync support, the Nvidia proprietary driver is not currently well supported (frame sharing will work, but you might experience tearing or frame pacing issues). This is a planned feature, but it will require work by OpenGL application developers (it cannot be made transparent in the API), so some apps might choose not to support it. In addition, frame sharing between an Nvidia GPU and a non-Nvidia GPU in OpenGL requires special support code and extra blitting, which will probably not be implemented until much later. If you have an Nvidia GPU, please consider using [NVK](https://docs.mesa3d.org/drivers/nvk.html) instead, since it implements all the missing features that Nvidia refuses to implement in their proprietary driver, and therefore doesn't require us application and library developers to add Nvidia-specific workaround code.

### Sending frames

The library initially targets sending frames to other applications (output). This covers the common use case of apps sending video to OBS for streaming.

### Receiving frames

Receiving is not implemented yet, but is in scope. Before jumping into this, I'd like to hear from maintainers of applications that receive video via Spout2/Syphon. If you maintain such an app (with existing or planned Linux support), please file an issue and let me know!

Of course, that doesn't make the sending side useless. To receive video streams in OBS, you can use [obs-pwvideo](https://github.com/hoshinolina/obs-pwvideo), which comes with its own PipeWire code (not using libfunnel itself).

It is also possible to hijack the input of any app using PipeWire screen sharing (such as Firefox, etc.) and redirect a libfunnel input stream into it, though this requires some hacky pw-cli shenanigans. If you have a use case for this, please let me know, as it should be possible to write a tool to do it mostly automatically.

### Multiple streams (multiple senders, multiple receivers, or combinations)

Multiple streams are supported the "obvious" way, but the streams behave completely independently (no synchronization) in the current implementation. For applications that synchronously process video, such as filters (input => output), there are better ways to do it. If you maintain an app that expects to input and/or output multiple video streams with locked frame sync, please file an issue and let me know your needs! The underlying PipeWire API is extremely powerful and complicated, and I need some example use cases to decide what a simplified libfunnel API for this should look like.

## Usage

TL;DR for EGL

```c

struct funnel_ctx *ctx;
struct funnel_stream *stream;

funnel_init(&ctx);
funnel_stream_create(ctx, "Funnel Test", &stream);
funnel_stream_init_egl(stream, egl_display);
funnel_stream_set_size(stream, width, height);

// FUNNEL_ASYNC           if you are rendering to the screen at screen FPS
//                        (in the same thread) and just sharing the frames,
// FUNNEL_DOUBLE_BUFFERED if you are rendering frames mainly for sharing
//                        and the frame rate is set by the consumer,
// FUNNEL_SINGLE_BUFFERED same but with lower latency,
// FUNNEL_SYNCHRONOUS     if you are just copying frames out of somewhere
//                        else on demand (like a Spout2 texture written by
//                        another process) and want zero added latency.
funnel_stream_set_mode(stream, FUNNEL_ASYNC);

// Optional: Enable explicit sync support
funnel_stream_set_sync(stream, FUNNEL_SYNC_EITHER);

// Formats in priority order
// If you don't want alpha, remove the first line
// Alternatively, you can just demote it (and make sure you always render
// 1.0 alpha in case it is chosen).
//
// Note: Alpha is always premultiplied. That's what you want, trust me.
funnel_stream_egl_add_format(stream, FUNNEL_EGL_FORMAT_RGBA8888);
funnel_stream_egl_add_format(stream, FUNNEL_EGL_FORMAT_RGB888);

funnel_stream_start(stream);

GLuint fb, color_tex;
glGenFramebuffers(1, &fb);

while (keep_rendering) {
    struct funnel_buffer *buf;

    // If you need to change the settings
    if (size_has_changed) {
        funnel_stream_set_size(stream, new_width, new_height);
        funnel_stream_configure(stream);
        // Change does not necessarily apply immediately, see below
    }

    funnel_stream_dequeue(stream, &buf);
    if (!buf) {
        // Skip this frame
        continue;
    }

    EGLImage image;
    funnel_buffer_get_egl_image(buf, &image);

    // Explicit sync support
    if (funnel_buffer_has_sync(buf)) {
        EGLSync acquire;
        funnel_buffer_get_acquire_egl_sync(buf, &acquire);
        eglWaitSync(egl_display, acquire, 0);
        eglDestroySync(egl_display, acquire);
    }

    // If the size might change, this is how you know the size
    // of this specific buffer you have to render to:
    funnel_buffer_get_size(buf, &width, &height);

    glGenTextures(1, &color_tex);
    glBindTexture(GL_TEXTURE_2D, color_tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER, fb);
    glFramebufferTexture2DEXT(GL_DRAW_FRAMEBUFFER,
                              GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D,
                              color_tex, 0);

    // Draw or blit your scene here!

    glFramebufferTexture2DEXT(GL_DRAW_FRAMEBUFFER,
                                GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D,
                                0, 0);
    glDeleteTextures(1, &color_tex);
    glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER, 0);

    // Explicit sync support
    if (funnel_buffer_has_sync(buf)) {
        EGLSync release = eglCreateSync(
            egl_display, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
        funnel_buffer_set_release_egl_sync(buf, release);
        eglDestroySync(egl_display, release);
    } else {
        // Required for implicit sync
        glFlush();
    }

    funnel_stream_enqueue(stream, buf);

}

funnel_stream_stop(stream);
funnel_stream_destroy(stream);
funnel_shutdown(ctx);
```
