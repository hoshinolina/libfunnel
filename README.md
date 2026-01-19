# libfunnel

A library to make creating PipeWire video streams easy, using zero-copy DMA-BUF frame sharing. "Spout2 / Syphon, but for Linux".

## Status

This is still rough around the edges and the API is not considered stable yet.

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
// FUNNEL_SYNC            if you are just copying frames out of somewhere
//                        else on demand (like a Spout2 texture written by
//                        another process) and want zero added latency.
funnel_stream_set_mode(stream, FUNNEL_ASYNC);

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

    glFlush();

    funnel_stream_enqueue(stream, buf);

}

funnel_stream_stop(stream);
funnel_stream_destroy(stream);
funnel_shutdown(ctx);
```
