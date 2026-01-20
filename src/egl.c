#include "funnel-egl.h"
#include "funnel-gbm.h"
#include "funnel_internal.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

PW_LOG_TOPIC_STATIC(log_funnel_egl, "funnel.egl");
#define PW_LOG_TOPIC_DEFAULT log_funnel_egl

static const struct {
    EGLAttrib fd, offset, pitch, modlo, modhi;
} egl_attributes[4] = {
    {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT,
     EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
     EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
    {EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
     EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
     EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
    {EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT,
     EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
     EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
    {EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT,
     EGL_DMA_BUF_PLANE3_PITCH_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
     EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT},
};

static void funnel_egl_alloc_buffer(struct funnel_buffer *buffer) {
    struct funnel_stream *stream = buffer->stream;

    int idx = 0;
    EGLAttrib attribute_list[7 + stream->cur.plane_count * 10];

    attribute_list[idx++] = EGL_WIDTH;
    attribute_list[idx++] = stream->cur.width;
    attribute_list[idx++] = EGL_HEIGHT;
    attribute_list[idx++] = stream->cur.height;
    attribute_list[idx++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribute_list[idx++] = stream->cur.format;

    for (int i = 0; i < stream->cur.plane_count; ++i) {
        attribute_list[idx++] = egl_attributes[i].fd;
        attribute_list[idx++] = buffer->fds[i];
        attribute_list[idx++] = egl_attributes[i].offset;
        attribute_list[idx++] = stream->cur.offsets[i],
        attribute_list[idx++] = egl_attributes[i].pitch;
        attribute_list[idx++] = stream->cur.strides[i],
        attribute_list[idx++] = egl_attributes[i].modlo;
        attribute_list[idx++] = stream->cur.modifier,
        attribute_list[idx++] = egl_attributes[i].modhi;
        attribute_list[idx++] = (uint32_t)(stream->cur.modifier >> 32);
    }
    attribute_list[idx++] = EGL_NONE;

    EGLImage image =
        eglCreateImage(stream->api_ctx, NULL, EGL_LINUX_DMA_BUF_EXT,
                       (EGLClientBuffer)NULL, attribute_list);
    assert(image != EGL_NO_IMAGE);

    buffer->api_buf = image;
}

static void funnel_egl_free_buffer(struct funnel_buffer *buffer) {
    eglDestroyImage(buffer->stream->api_ctx, buffer->api_buf);
}

static const struct funnel_stream_funcs egl_funcs = {
    .alloc_buffer = funnel_egl_alloc_buffer,
    .free_buffer = funnel_egl_free_buffer,
};

static PFNEGLQUERYDEVICESTRINGEXTPROC eglQueryDeviceStringEXT;
static PFNEGLQUERYDISPLAYATTRIBEXTPROC eglQueryDisplayAttribEXT;
static PFNEGLQUERYDMABUFMODIFIERSEXTPROC eglQueryDmaBufModifiersEXT;
static PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID;

int funnel_stream_init_egl(struct funnel_stream *stream, EGLDisplay display) {
    if (stream->api != API_UNSET)
        return -EEXIST;

    if (!eglQueryDeviceStringEXT)
        eglQueryDeviceStringEXT =
            (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress(
                "eglQueryDeviceStringEXT");
    if (!eglQueryDisplayAttribEXT)
        eglQueryDisplayAttribEXT =
            (PFNEGLQUERYDISPLAYATTRIBEXTPROC)eglGetProcAddress(
                "eglQueryDisplayAttribEXT");
    if (!eglQueryDmaBufModifiersEXT)
        eglQueryDmaBufModifiersEXT =
            (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress(
                "eglQueryDmaBufModifiersEXT");
    if (!eglDupNativeFenceFDANDROID)
        eglDupNativeFenceFDANDROID =
            (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress(
                "eglDupNativeFenceFDANDROID");

    assert(eglQueryDeviceStringEXT);
    assert(eglQueryDisplayAttribEXT);
    assert(eglQueryDmaBufModifiersEXT);

    EGLAttrib device_attr = 0;
    if (!eglQueryDisplayAttribEXT(display, EGL_DEVICE_EXT, &device_attr) ||
        !device_attr) {
        pw_log_error("failed to query EGLDeviceExt");
        return -ENODEV;
    }

    EGLDeviceEXT device = (EGLDeviceEXT *)device_attr;

    const char *render_node =
        eglQueryDeviceStringEXT(device, EGL_DRM_RENDER_NODE_FILE_EXT);

    if (!render_node)
        render_node = eglQueryDeviceStringEXT(device, EGL_DRM_DEVICE_FILE_EXT);

    if (!render_node) {
        pw_log_error("failed to get device node");
        return -ENODEV;
    }

    pw_log_info("DRM render node: %s", render_node);

    int gbm_fd = open(render_node, O_RDWR);
    if (gbm_fd < 0) {
        pw_log_error("failed to open device node %s: %d", render_node, errno);
        return -errno;
    }

    int ret = funnel_stream_init_gbm(stream, gbm_fd);
    close(gbm_fd);

    if (ret < 0)
        return ret;

    stream->funcs = &egl_funcs;
    stream->api = API_EGL;
    stream->api_ctx = display;

    if (!eglDupNativeFenceFDANDROID)
        stream->api_supports_explicit_sync = false;
    stream->api_requires_explicit_sync = false;

    return 0;
}

static bool try_format(struct funnel_stream *stream, uint32_t format) {
    EGLint count;
    if (eglQueryDmaBufModifiersEXT(stream->api_ctx, format, 0, NULL, NULL,
                                   &count) != EGL_TRUE) {
        return false;
    }

    EGLuint64KHR *modifiers = malloc(sizeof(EGLuint64KHR) * count);
    EGLBoolean *external = malloc(sizeof(EGLBoolean) * count);

    assert(eglQueryDmaBufModifiersEXT(stream->api_ctx, format, count, modifiers,
                                      external, &count));

    pw_log_info("Check format: 0x%x [%d modifiers]", format, count);
    for (unsigned i = 0; i < count; i++) {
        pw_log_info(" - 0x%llx [external=%d]", (long long)modifiers[i],
                    external[i]);
    }

    for (unsigned i = 0, j = 0; i < count; j++) {
        if (external[j]) {
            memmove(&modifiers[i], &modifiers[i + 1],
                    sizeof(uint64_t) * (count - i - 1));
            count--;
        } else {
            i++;
        }
    }

    int ret = -ENOENT;
    if (count) {
        pw_log_info("%d usable modifiers", count);
        ret = funnel_stream_gbm_add_format(stream, format, modifiers, count);
    }

    free(modifiers);
    free(external);

    return ret >= 0;
}

int funnel_stream_egl_add_format(struct funnel_stream *stream,
                                 enum funnel_egl_format format) {
    bool success = false;
    if (stream->api != API_EGL)
        return -EINVAL;

    switch (format) {
    case FUNNEL_EGL_FORMAT_RGB888:
        success |= try_format(stream, GBM_FORMAT_XRGB8888);
        success |= try_format(stream, GBM_FORMAT_RGBX8888);
        success |= try_format(stream, GBM_FORMAT_XBGR8888);
        success |= try_format(stream, GBM_FORMAT_BGRX8888);
        break;
    case FUNNEL_EGL_FORMAT_RGBA8888:
        success |= try_format(stream, GBM_FORMAT_ARGB8888);
        success |= try_format(stream, GBM_FORMAT_RGBA8888);
        success |= try_format(stream, GBM_FORMAT_ABGR8888);
        success |= try_format(stream, GBM_FORMAT_BGRA8888);
        break;
    default:
        return -EINVAL;
    }

    return success ? 0 : -ENOTSUP;
}

int funnel_buffer_get_egl_image(struct funnel_buffer *buf, EGLImage *image) {
    *image = NULL;
    if (!buf || buf->stream->api != API_EGL)
        return -EINVAL;

    *image = buf->api_buf;
    return 0;
}

int funnel_buffer_get_egl_format(struct funnel_buffer *buf,
                                 enum funnel_egl_format *format) {
    *format = FUNNEL_EGL_FORMAT_UNKNOWN;

    if (!buf || buf->stream->api != API_EGL)
        return -EINVAL;

    switch (gbm_bo_get_format(buf->bo)) {
    case GBM_FORMAT_ARGB8888:
    case GBM_FORMAT_RGBA8888:
    case GBM_FORMAT_ABGR8888:
    case GBM_FORMAT_BGRA8888:
        *format = FUNNEL_EGL_FORMAT_RGBA8888;
        break;
    case GBM_FORMAT_XRGB8888:
    case GBM_FORMAT_RGBX8888:
    case GBM_FORMAT_XBGR8888:
    case GBM_FORMAT_BGRX8888:
        *format = FUNNEL_EGL_FORMAT_RGB888;
        break;
    }
    return 0;
}

int funnel_buffer_get_acquire_egl_sync(struct funnel_buffer *buf,
                                       EGLSync *sync) {
    int fd;

    if (!buf || buf->stream->api != API_EGL)
        return -EINVAL;

    int ret = funnel_buffer_get_acquire_sync_file(buf, &fd);
    if (ret < 0)
        return ret;

    EGLAttrib attributes[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fd, EGL_NONE};

    *sync = eglCreateSync(buf->stream->api_ctx, EGL_SYNC_NATIVE_FENCE_ANDROID,
                          attributes);
    if (*sync == EGL_NO_SYNC) {
        pw_log_error("Unable to create an acquire EGLSync");
        return -EIO;
        close(fd);
    }

    return 0;
}

int funnel_buffer_set_release_egl_sync(struct funnel_buffer *buf,
                                       EGLSync sync) {

    if (!buf || buf->stream->api != API_EGL)
        return -EINVAL;

    int fd = eglDupNativeFenceFDANDROID(buf->stream->api_ctx, sync);
    if (fd == EGL_NO_NATIVE_FENCE_FD_ANDROID) {
        pw_log_error("Unable to get the release sync fd, is this an "
                     "EGL_SYNC_NATIVE_FENCE_ANDROID?");
        return -EINVAL;
    }

    int ret = funnel_buffer_set_release_sync_file(buf, fd);
    close(fd);
    return ret;
}
