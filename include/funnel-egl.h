#pragma once

#include "funnel.h"
#include <EGL/egl.h>

enum funnel_egl_format {
    FUNNEL_EGL_FORMAT_UNKNOWN = 0,
    FUNNEL_EGL_FORMAT_RGB888,
    FUNNEL_EGL_FORMAT_RGBA8888,
};

/**
 * Set up a stream for EGL integration.
 *
 * @param stream Stream
 * @param display EGLDisplay to attach to the stream (must outlive stream)
 */
int funnel_stream_init_egl(struct funnel_stream *stream, EGLDisplay display);

/**
 * Add a supported EGL format. Must be called in preference order (highest to
 * lowest).
 *
 * @param stream Stream
 * @param format `enum funnel_egl_format` format
 */
int funnel_stream_egl_add_format(struct funnel_stream *stream,
                                 enum funnel_egl_format format);

/**
 * Get the EGLImage for a Funnel buffer.
 *
 * The EGLImage will only be valid until `buf` is returned or enqueued, or the
 * stream is destroyed.
 *
 * @param buf Buffer
 * @param bo Output EGLImage for the buffer (borrowed)
 */
int funnel_buffer_get_egl_image(struct funnel_buffer *buf, EGLImage *image);

/**
 * Get the EGL format for a Funnel buffer.
 *
 * @param buf Buffer
 * @param format Output EGL format
 */
int funnel_buffer_get_egl_format(struct funnel_buffer *buf,
                                 enum funnel_egl_format *format);

/**
 * Get the EGLSync for acquiring the buffer.
 *
 * The user must wait on this sync object before accessing
 * the buffer.
 *
 * @param buf Buffer
 * @param sync Output EGLSync (must be destroyed by caller)
 */
int funnel_buffer_get_acquire_egl_sync(struct funnel_buffer *buf,
                                       EGLSync *sync);

/**
 * Set the EGLSync for releasing the buffer.
 *
 * This sync object must be signaled when access to the buffer is complete.
 * The sync type must be EGL_SYNC_NATIVE_FENCE_ANDROID.
 *
 * @param buf Buffer
 * @param sync EGLSync (borrowed, must be destroyed by caller)
 */
int funnel_buffer_set_release_egl_sync(struct funnel_buffer *buf, EGLSync sync);
