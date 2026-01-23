#include "funnel.h"
#include "funnel-gbm.h"
#include "funnel_internal.h"
#include "pipewire/stream.h"

#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libdrm/drm_fourcc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <spa/debug/format.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/param/props.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/dynamic.h>
#include <spa/pod/filter.h>
#include <spa/utils/result.h>

#include <gbm.h>
#include <linux/dma-buf.h>
#include <pipewire/log.h>
#include <pipewire/pipewire.h>
#include <xf86drm.h>

PW_LOG_TOPIC_STATIC(log_funnel, "funnel.core");
#define PW_LOG_TOPIC_DEFAULT log_funnel

static struct {
    uint32_t drm_format;
    enum spa_video_format spa_format;
} supported_formats[] = {
    {
        .drm_format = GBM_FORMAT_ARGB8888,
        .spa_format = SPA_VIDEO_FORMAT_BGRA,
    },
    {
        .drm_format = GBM_FORMAT_RGBA8888,
        .spa_format = SPA_VIDEO_FORMAT_ABGR,
    },
    {
        .drm_format = GBM_FORMAT_ABGR8888,
        .spa_format = SPA_VIDEO_FORMAT_RGBA,
    },
    {
        .drm_format = GBM_FORMAT_BGRA8888,
        .spa_format = SPA_VIDEO_FORMAT_ARGB,
    },
    {
        .drm_format = GBM_FORMAT_XRGB8888,
        .spa_format = SPA_VIDEO_FORMAT_BGRx,
    },
    {
        .drm_format = GBM_FORMAT_RGBX8888,
        .spa_format = SPA_VIDEO_FORMAT_xBGR,
    },
    {
        .drm_format = GBM_FORMAT_XBGR8888,
        .spa_format = SPA_VIDEO_FORMAT_RGBx,
    },
    {
        .drm_format = GBM_FORMAT_BGRX8888,
        .spa_format = SPA_VIDEO_FORMAT_xRGB,
    },
};

///////////////////////////////////////////////

static int funnel_stream_import_sync_file(struct funnel_stream *stream,
                                          uint32_t handle, int fd,
                                          uint64_t point);
static int funnel_stream_export_sync_file(struct funnel_stream *stream,
                                          uint32_t handle, uint64_t point,
                                          int *fd);

static void free_params(const struct spa_pod **params, size_t count) {
    for (size_t i = 0; i < count; i++)
        free((void *)params[i]);
}

static int build_formats(struct funnel_stream *stream, bool fixate,
                         const struct spa_pod **params);

static void on_core_error(void *data, uint32_t id, int seq, int res,
                          const char *message) {
    struct funnel_ctx *ctx = data;

    pw_log_error("error id:%u seq:%d res:%d (%s): %s", id, seq, res,
                 spa_strerror(res), message);

    if (id == PW_ID_CORE) {
        ctx->dead = true;
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

static void on_add_buffer(void *data, struct pw_buffer *pwbuffer) {
    struct funnel_stream *stream = data;

    int flags = GBM_BO_USE_RENDERING;

    struct spa_data *spa_data = pwbuffer->buffer->datas;
    assert(spa_data[0].type & (1 << SPA_DATA_DmaBuf));

    struct gbm_bo *bo = NULL;

    bo = gbm_bo_create_with_modifiers2(stream->gbm, stream->cur.aligned_width,
                                       stream->cur.height, stream->cur.format,
                                       &stream->cur.modifier, 1, flags);

    assert(bo);

    struct funnel_buffer *buffer = calloc(1, sizeof(struct funnel_buffer));
    buffer->pw_buffer = pwbuffer;
    buffer->stream = stream;
    buffer->bo = bo;
    buffer->width = stream->cur.width;
    buffer->height = stream->cur.height;

    buffer->backend_sync = false; // TODO

    buffer->frontend_sync = buffer->backend_sync;
    if (!buffer->backend_sync &&
        stream->cur.config.sync == FUNNEL_SYNC_EXPLICIT_HYBRID) {
        int fd = gbm_device_get_fd(stream->gbm);

        int ret = drmSyncobjCreate(fd, 0, &buffer->acquire.handle);
        assert(ret >= 0);

        ret = drmSyncobjCreate(fd, 0, &buffer->release.handle);
        assert(ret >= 0);

        buffer->acquire.point = 0;
        buffer->release.point = 0;
        buffer->frontend_sync = true;
    }

    pw_log_debug("on_add_buffer: %p -> %p", pwbuffer, buffer);

    for (int i = 0; i < ARRAY_SIZE(buffer->fds); i++) {
        buffer->fds[i] = -1;
    }

    pwbuffer->user_data = buffer;

    for (int i = 0; i < stream->cur.plane_count; ++i) {
        spa_data[i].type = SPA_DATA_DmaBuf;
        spa_data[i].flags = SPA_DATA_FLAG_READWRITE;
        spa_data[i].mapoffset = 0;
        spa_data[i].maxsize =
            i == 0 ? stream->cur.strides[i] * stream->cur.height : 0;
        spa_data[i].fd = buffer->fds[i] = gbm_bo_get_fd(bo);
        spa_data[i].data = NULL;
        spa_data[i].chunk->offset = stream->cur.offsets[i];
        spa_data[i].chunk->size = spa_data[i].maxsize;
        spa_data[i].chunk->stride = stream->cur.strides[i];
        spa_data[i].chunk->flags = SPA_CHUNK_FLAG_NONE;
    };

    if (stream->funcs)
        stream->funcs->alloc_buffer(buffer);

    if (stream->alloc_cb)
        stream->alloc_cb(stream->cb_opaque, stream, buffer);

    stream->num_buffers++;
}

static void funnel_buffer_free(struct funnel_buffer *buffer) {
    struct funnel_stream *stream = buffer->stream;
    if (stream->free_cb)
        stream->free_cb(stream->cb_opaque, stream, buffer);
    if (stream->funcs)
        stream->funcs->free_buffer(buffer);

    gbm_bo_destroy(buffer->bo);
    for (int i = 0; i < ARRAY_SIZE(buffer->fds); i++) {
        if (buffer->fds[i] >= 0)
            close(buffer->fds[i]);
    }

    if (!buffer->backend_sync &&
        stream->cur.config.sync == FUNNEL_SYNC_EXPLICIT_HYBRID) {
        int fd = gbm_device_get_fd(stream->gbm);

        int ret = drmSyncobjDestroy(fd, buffer->acquire.handle);
        assert(ret >= 0);
        ret = drmSyncobjDestroy(fd, buffer->release.handle);
        assert(ret >= 0);
    }

    free(buffer);
}

static inline bool is_buffer_pending(struct funnel_stream *stream) {
    assert(!(stream->pending_buffer && stream->skip_buffer));
    return stream->pending_buffer || stream->skip_buffer;
}

static void on_remove_buffer(void *data, struct pw_buffer *pwbuffer) {
    pw_log_debug("on_remove_buffer: %p -> %p", pwbuffer, pwbuffer->user_data);

    if (pwbuffer->user_data) {
        struct funnel_buffer *buffer = pwbuffer->user_data;
        struct funnel_stream *stream = buffer->stream;

        if (!buffer->dequeued) {
            funnel_buffer_free(buffer);
            if (buffer == stream->pending_buffer)
                stream->pending_buffer = NULL;
        } else {
            buffer->pw_buffer = NULL;
            pw_log_debug("defer buffer free: %p", buffer);
        }

        pwbuffer->user_data = NULL;
        stream->num_buffers--;
    }
}

static void update_timeouts(struct funnel_stream *stream) {
    struct timespec timeout, interval, *to, *iv;
    enum pw_stream_state state = pw_stream_get_state(stream->stream, NULL);

    bool timeouts_active = false;

    if (state == PW_STREAM_STATE_STREAMING &&
        pw_stream_is_driving(stream->stream) &&
        !pw_stream_is_lazy(stream->stream) &&
        stream->cur.config.mode != FUNNEL_ASYNC)
        timeouts_active = true;

    if (!timeouts_active) {
        to = iv = NULL;
    } else {
        struct spa_fraction rate = stream->cur.video_format.framerate;

        if (rate.num == 0 || rate.denom == 0) {
            // Pick a default rate of 60 FPS
            rate.num = 60;
            rate.denom = 1;
            pw_log_debug("default rate: 60 FPS");
        } else {
            pw_log_debug("negotiated rate: %d/%d FPS", rate.num, rate.denom);
        }
        uint64_t nsec = rate.denom * 1000000000L / rate.num;

        timeout.tv_sec = 0;
        timeout.tv_nsec = 1;
        interval.tv_sec = nsec / 1000000000L;
        interval.tv_nsec = nsec % 1000000000L;
        to = &timeout;
        iv = &interval;
    }
    pw_loop_update_timer(pw_thread_loop_get_loop(stream->ctx->loop),
                         stream->timer, to, iv, false);
}

static int return_buffer(struct funnel_stream *stream,
                         struct funnel_buffer *buf) {
    if (!buf->pw_buffer) {
        funnel_buffer_free(buf);
        return -ESTALE;
    }

    return pw_stream_return_buffer(stream->stream, buf->pw_buffer);
}

static void reset_buffers(struct funnel_stream *stream) {
    if (stream->pending_buffer) {
        return_buffer(stream, stream->pending_buffer);
        stream->pending_buffer = NULL;
    }
    stream->skip_buffer = false;
}

static void on_state_changed(void *data, enum pw_stream_state old,
                             enum pw_stream_state state,
                             const char *error_message) {
    struct funnel_stream *stream = data;

    pw_log_info("on_state_changed: %s -> %s %s", pw_stream_state_as_string(old),
                pw_stream_state_as_string(state), error_message);
    switch (state) {
    case PW_STREAM_STATE_ERROR:
        reset_buffers(stream);
        break;
    case PW_STREAM_STATE_PAUSED:
        reset_buffers(stream);
        update_timeouts(stream);
        break;
    case PW_STREAM_STATE_STREAMING:
        pw_log_info("driving:%d lazy:%d", pw_stream_is_driving(stream->stream),
                    pw_stream_is_lazy(stream->stream));
        update_timeouts(stream);
        break;
    case PW_STREAM_STATE_CONNECTING:
        update_timeouts(stream);
        reset_buffers(stream);
        break;
    case PW_STREAM_STATE_UNCONNECTED:
        update_timeouts(stream);
        reset_buffers(stream);
        break;
    }
}

static bool test_create_dmabuf(struct funnel_stream *stream, uint32_t format,
                               uint64_t *modifiers, size_t num_modifiers) {
    int flags = GBM_BO_USE_RENDERING;

    struct gbm_bo *bo;

    bo = gbm_bo_create_with_modifiers2(stream->gbm,
                                       stream->cur.video_format.size.width,
                                       stream->cur.video_format.size.height,
                                       format, modifiers, num_modifiers, flags);
    if (!bo)
        return false;

    stream->cur.width = gbm_bo_get_width(bo);
    stream->cur.height = gbm_bo_get_height(bo);
    assert(stream->cur.width == stream->cur.video_format.size.width);
    assert(stream->cur.height == stream->cur.video_format.size.height);

    if (gbm_bo_get_modifier(bo) == DRM_FORMAT_MOD_LINEAR) {
        gbm_bo_destroy(bo);

        // Align linear buffers to 64 pixel width (256 bytes for 32-bit format)
        // for cross-GPU compatibility
        stream->cur.aligned_width = (stream->cur.width + 63) & ~63;

        const uint64_t mod = DRM_FORMAT_MOD_LINEAR;

        bo = gbm_bo_create_with_modifiers2(
            stream->gbm, stream->cur.aligned_width, stream->cur.height, format,
            &mod, 1, flags);

        if (!bo)
            return false;

        assert(gbm_bo_get_width(bo) == stream->cur.aligned_width);
        assert(gbm_bo_get_height(bo) == stream->cur.height);
    } else {
        stream->cur.aligned_width = stream->cur.width;
    }

    stream->cur.plane_count = gbm_bo_get_plane_count(bo);
    for (int i = 0; i < stream->cur.plane_count; i++) {
        stream->cur.strides[i] = gbm_bo_get_stride_for_plane(bo, i);
        stream->cur.offsets[i] = gbm_bo_get_offset(bo, i);
    }
    stream->cur.format = gbm_bo_get_format(bo);
    stream->cur.modifier = gbm_bo_get_modifier(bo);

    gbm_bo_destroy(bo);

    return true;
}

static void on_param_changed(void *data, uint32_t id,
                             const struct spa_pod *format) {
    pw_log_debug("on_param_changed: %d %p", id, format);

    struct funnel_stream *stream = data;

    if (!format || id != SPA_PARAM_Format) {
        pw_log_debug(" ->ignored");
        return;
    }

    int i;
    uint32_t dmabuf_format;

    spa_format_video_raw_parse(format, &stream->cur.video_format);

    for (i = 0; i < ARRAY_SIZE(supported_formats); i++) {
        if (supported_formats[i].spa_format ==
            stream->cur.video_format.format) {
            dmabuf_format = supported_formats[i].drm_format;
            break;
        }
    }
    if (i >= ARRAY_SIZE(supported_formats)) {
        pw_log_error("unsupported format %d", stream->cur.video_format.format);
        return;
    }

    const struct spa_pod_prop *mod_prop =
        spa_pod_find_prop(format, NULL, SPA_FORMAT_VIDEO_modifier);

    assert(mod_prop);

    const uint32_t value_count = SPA_POD_CHOICE_N_VALUES(&mod_prop->value);
    const uint64_t *values = (uint64_t *)SPA_POD_CHOICE_VALUES(
        &mod_prop->value); // values[0] is the preferred choice

    int mod_count = 0;
    uint64_t *modifiers = malloc(value_count * sizeof(uint64_t));

    for (int i = 0; i < value_count; i++) {
        bool found = false;
        for (int j = 0; j < mod_count; j++) {
            if (values[i] == modifiers[j]) {
                found = true;
                break;
            }
        }
        if (!found) {
            modifiers[mod_count++] = values[i];
        }
    }

    if (mod_count > 1) {
        for (int j = 0; j < mod_count; j++) {
            if (modifiers[j] == DRM_FORMAT_MOD_INVALID) {
                mod_count--;
                memmove(&modifiers[j], &modifiers[j + 1], mod_count - j);
                break;
            }
        }
    }

    if (stream->cur.width != stream->cur.video_format.size.width ||
        stream->cur.height != stream->cur.video_format.size.height ||
        stream->cur.format != dmabuf_format) {

        if (!test_create_dmabuf(stream, dmabuf_format, modifiers, mod_count)) {
            pw_log_error("failed to create dmabuf for format 0x%x",
                         dmabuf_format);
            return;
        }

        pw_log_info("Created buffer with format 0x%x and modifier 0x%llx "
                    "(%dx%d %dp s=%d o=%d)",
                    stream->cur.format, (long long)stream->cur.modifier,
                    stream->cur.width, stream->cur.height,
                    stream->cur.plane_count, stream->cur.strides[0],
                    stream->cur.offsets[1]);

        size_t num_formats =
            pw_array_get_len(&stream->cur.config.formats, struct funnel_format);

        const struct spa_pod **params =
            calloc(num_formats + 1, sizeof(struct spa_pod *));

        int num_params = build_formats(stream, true, params);
        assert(num_params <= (num_formats + 1));

        stream->cur.ready = false;
        pw_stream_update_params(stream->stream, params, num_params);
        free_params(params, num_params);
        return;
    }

    const int buffertypes = (1 << SPA_DATA_DmaBuf);

    spa_auto(spa_pod_dynamic_builder) pod_builder = {0};
    struct spa_pod_frame f;
    spa_pod_dynamic_builder_init(&pod_builder, NULL, 0, 1024);

    int num_params = 0;
    const struct spa_pod *params[8];

    // Fallback buffer parameters for DmaBuf with implicit sync or MemFd
    spa_pod_builder_push_object(
        &pod_builder.b, &f, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
    spa_pod_builder_add(
        &pod_builder.b, SPA_PARAM_BUFFERS_buffers,
        SPA_POD_CHOICE_RANGE_Int(stream->cur.config.buffers.def,
                                 stream->cur.config.buffers.min,
                                 stream->cur.config.buffers.max),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(buffertypes), 0);
    spa_pod_builder_add(&pod_builder.b, SPA_PARAM_BUFFERS_blocks,
                        SPA_POD_Int(stream->cur.plane_count), 0);
    params[num_params++] =
        (struct spa_pod *)spa_pod_builder_pop(&pod_builder.b, &f);

    params[num_params++] = (struct spa_pod *)spa_pod_builder_add_object(
        &pod_builder.b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size,
        SPA_POD_Int(sizeof(struct spa_meta_header)));

    pw_stream_update_params(stream->stream, params, num_params);
    stream->cur.ready = true;
}

static void on_command(void *data, const struct spa_command *command) {
    struct funnel_stream *stream = data;

    switch (SPA_NODE_COMMAND_ID(command)) {
    case SPA_NODE_COMMAND_RequestProcess:
        if (!pw_stream_is_lazy(stream->stream))
            return;
        pw_log_trace("RequestProcess %p", stream);
        pw_stream_trigger_process(stream->stream);
        break;
    default:
        break;
    }
}

static void unblock_process_thread(struct funnel_stream *stream) {
    if (stream->cycle_state == SYNC_CYCLE_ACTIVE) {
        pw_thread_loop_accept(stream->ctx->loop);
    }
    stream->cycle_state = SYNC_CYCLE_INACTIVE;
}

static void on_process(void *data) {
    struct funnel_stream *stream = data;

    pw_log_trace("BEGIN frame %d", ++stream->frame);

    if (!stream->active)
        return;

    if (stream->cur.config.mode == FUNNEL_SYNCHRONOUS) {
        // Sync mode handshake
        if (stream->cycle_state == SYNC_CYCLE_WAITING) {
            stream->cycle_state = SYNC_CYCLE_ACTIVE;
            pw_log_trace("Signal sync");
            pw_thread_loop_signal(stream->ctx->loop, true);
            pw_log_trace("Accepted");
        }
        // We should have a buffer now, if the cycle succeeded
    }

    if (stream->pending_buffer) {
        struct funnel_buffer *buf = stream->pending_buffer;
        stream->pending_buffer = NULL;

        assert(buf->pw_buffer);
        pw_log_trace("Queued buffer");
        pw_stream_queue_buffer(stream->stream, buf->pw_buffer);
    } else if (stream->skip_buffer) {
        stream->skip_buffer = false;
    }

    pw_thread_loop_signal(stream->ctx->loop, false);

    pw_log_trace("END frame %d", stream->frame);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .add_buffer = on_add_buffer,
    .remove_buffer = on_remove_buffer,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .command = on_command,
    .process = on_process,
};

static void on_timeout(void *userdata, uint64_t expirations) {
    struct funnel_stream *stream = userdata;

    pw_log_trace("Timeout %p", stream);
    pw_stream_trigger_process(stream->stream);
}

static struct spa_pod *
build_format(enum spa_video_format format, struct spa_rectangle *resolution,
             struct spa_fraction *def_rate, struct spa_fraction *min_rate,
             struct spa_fraction *max_rate, const uint64_t *modifiers,
             size_t num_modifiers, uint32_t modifiers_flags) {
    struct spa_pod_frame f[2];

    struct spa_pod_dynamic_builder pod_builder;
    spa_pod_dynamic_builder_init(&pod_builder, NULL, 0, 1024);
    struct spa_pod_builder *b = &pod_builder.b;

    spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format,
                                SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b, SPA_FORMAT_mediaType,
                        SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype,
                        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(resolution),
                        0);
    // spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate,
    //                     SPA_POD_Fraction(def_rate), 0);
    spa_pod_builder_add(
        b, SPA_FORMAT_VIDEO_framerate,
        SPA_POD_CHOICE_RANGE_Fraction(def_rate, min_rate, max_rate), 0);
    spa_pod_builder_add(
        b, SPA_FORMAT_VIDEO_maxFramerate,
        SPA_POD_CHOICE_RANGE_Fraction(def_rate, min_rate, max_rate), 0);

    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);

    if (num_modifiers) {
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, modifiers_flags);
        spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);

        for (size_t i = 0; i < num_modifiers; i++) {
            spa_pod_builder_long(b, modifiers[i]);
            if (i == 0) {
                spa_pod_builder_long(b, modifiers[i]);
            }
        }
        spa_pod_builder_pop(b, &f[1]);
    }
    return (struct spa_pod *)spa_pod_builder_pop(b, &f[0]);
}

static int build_formats(struct funnel_stream *stream, bool fixate,
                         const struct spa_pod **params) {
    struct funnel_stream_config *config = &stream->cur.config;

    struct spa_fraction def_rate = to_spa_fraction(config->rate.def);
    struct spa_fraction min_rate = to_spa_fraction(config->rate.min);
    struct spa_fraction max_rate = to_spa_fraction(config->rate.max);

    struct spa_rectangle resolution =
        SPA_RECTANGLE(config->width, config->height);

    int num_params = 0;
    if (fixate) {
        num_params++;
        *params++ = build_format(
            stream->cur.video_format.format, &resolution, &def_rate, &min_rate,
            &max_rate, &stream->cur.modifier, 1, SPA_POD_PROP_FLAG_MANDATORY);
    }

    struct funnel_format *format;
    pw_array_for_each (format, &config->formats) {
        num_params++;
        *params++ = build_format(
            format->spa_format, &resolution, &def_rate, &min_rate, &max_rate,
            format->modifiers, format->num_modifiers,
            SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
    }

    return num_params;
}

int funnel_init(struct funnel_ctx **pctx) {
    struct funnel_ctx *ctx;

    *pctx = NULL;
    ctx = calloc(1, sizeof(*ctx));
    assert(ctx);

    pw_init(NULL, NULL);

    ctx->loop = pw_thread_loop_new("funnel_loop", NULL);
    assert(ctx->loop);

    pw_thread_loop_lock(ctx->loop);

    pw_thread_loop_start(ctx->loop);

    ctx->context = pw_context_new(pw_thread_loop_get_loop(ctx->loop), NULL, 0);
    assert(ctx->context);

    if ((ctx->core = pw_context_connect(ctx->context, NULL, 0)) == NULL) {
        pw_log_error("failed to connect to PipeWire");
        pw_thread_loop_unlock(ctx->loop);
        funnel_shutdown(ctx);
        return -ECONNREFUSED;
    }

    pw_core_add_listener(ctx->core, &ctx->core_listener, &core_events, ctx);

    pw_thread_loop_unlock(ctx->loop);

    *pctx = ctx;
    return 0;
}

void funnel_shutdown(struct funnel_ctx *ctx) {
    if (!ctx)
        return;

    assert(ctx->loop);

    /* Thread loop should be unlocked here */
    pw_thread_loop_stop(ctx->loop);

    if (ctx->core)
        pw_core_disconnect(ctx->core);

    if (ctx->context)
        pw_context_destroy(ctx->context);

    pw_thread_loop_destroy(ctx->loop);

    free(ctx);
    pw_deinit();
}

int funnel_stream_create(struct funnel_ctx *ctx, const char *name,
                         struct funnel_stream **pstream) {
    struct funnel_stream *stream;
    assert(ctx);

    pw_thread_loop_lock(ctx->loop);

    if (ctx->dead)
        UNLOCK_RETURN(-EIO);

    *pstream = NULL;
    stream = calloc(1, sizeof(*stream));
    assert(stream);

    stream->ctx = ctx;
    stream->name = strdup(name);

    funnel_stream_set_mode(stream, FUNNEL_ASYNC);
    funnel_stream_set_sync(stream, FUNNEL_SYNC_IMPLICIT);

    stream->config.rate.def = FUNNEL_RATE_VARIABLE;
    stream->config.rate.min = FUNNEL_RATE_VARIABLE;
    stream->config.rate.max = FUNNEL_RATE_VARIABLE;

    stream->config_pending = true;

    pw_array_init(&stream->config.formats, 32);
    pw_array_init(&stream->cur.config.formats, 32);

    stream->timer = pw_loop_add_timer(pw_thread_loop_get_loop(ctx->loop),
                                      on_timeout, stream);
    assert(stream->timer);

    *pstream = stream;

    UNLOCK_RETURN(0);
}

void funnel_stream_set_buffer_callbacks(struct funnel_stream *stream,
                                        funnel_buffer_callback alloc,
                                        funnel_buffer_callback free,
                                        void *opaque) {
    stream->alloc_cb = alloc;
    stream->free_cb = free;
    stream->cb_opaque = opaque;
}

int funnel_stream_init_gbm(struct funnel_stream *stream, int gbm_fd) {
    if (stream->gbm)
        return -EEXIST;

    if (stream->api != API_UNSET)
        return -EEXIST;

    int fd = fcntl(gbm_fd, F_DUPFD_CLOEXEC, 0);
    assert(fd >= 0);

    stream->gbm = gbm_create_device(fd);
    if (!stream->gbm)
        return -EINVAL;

    stream->gbm_timeline_sync = false;

    uint64_t cap;
    int ret = drmGetCap(fd, DRM_CAP_SYNCOBJ_TIMELINE, &cap);
    stream->gbm_timeline_sync = cap && !ret;
    stream->gbm_timeline_sync_import_export = false;

#ifdef DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE
    if (stream->gbm_timeline_sync) {

        // Test for DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE support
        struct drm_syncobj_handle args = {
            .handle = 0, // invalid
            .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE |
                     DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE,
        };

        ret = drmIoctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &args);

        assert(ret == -1);
        if (errno == ENOENT) {
            // Syncobj does not exist, but flags are supported
            stream->gbm_timeline_sync_import_export = true;
        } else {
            // Create a dummy syncobj to use for transfers
            int ret = drmSyncobjCreate(fd, 0, &stream->dummy_syncobj);
            assert(ret >= 0);
        }
    }

    pw_log_info("GBM features: fd=%d timeline_sync=%d, import_export=%d", fd,
                stream->gbm_timeline_sync,
                stream->gbm_timeline_sync_import_export);
#endif

    stream->api = API_GBM;
    stream->api_supports_explicit_sync = stream->gbm_timeline_sync;
    stream->api_requires_explicit_sync = false;
    return 0;
}

/**
 * Add a supported GBM format. Must be called in preference order (highest to
 * lowest).
 *
 * @param stream Stream
 * @param format DRM format (FOURCC)
 * @param modifiers Pointer to a list of modifiers (borrow)
 * @param num_modifiers Number of modifiers passed
 */
int funnel_stream_gbm_add_format(struct funnel_stream *stream, uint32_t format,
                                 uint64_t *modifiers, size_t num_modifiers) {
    int i;
    enum spa_video_format spa_format;

    if (!num_modifiers)
        return -EINVAL;

    for (i = 0; i < ARRAY_SIZE(supported_formats); i++) {
        if (supported_formats[i].drm_format == format) {
            spa_format = supported_formats[i].spa_format;
            break;
        }
    }
    if (i >= ARRAY_SIZE(supported_formats)) {
        return -ENOTSUP;
    }

    struct funnel_format *fmt =
        pw_array_add(&stream->config.formats, sizeof(struct funnel_format));
    assert(fmt);

    bool nonlinear = false;
    for (int i = 0; i < num_modifiers; i++)
        if (modifiers[i] != DRM_FORMAT_MOD_LINEAR)
            nonlinear = true;

    fmt->format = format;
    fmt->spa_format = spa_format;
    fmt->modifiers = calloc(num_modifiers, sizeof(uint64_t));
    fmt->num_modifiers = num_modifiers;
    memcpy(fmt->modifiers, modifiers, num_modifiers * sizeof(uint64_t));
    pw_log_info("Add format 0x%x: modifiers=%p fmt=%p base=%p nonlinear=%d",
                format, fmt->modifiers, fmt, stream->config.formats.data,
                nonlinear);

    if (nonlinear)
        stream->config.has_nonlinear_tiling = true;

    stream->config_pending = true;
    return 0;
}

static void funnel_reset_formats(struct pw_array *formats) {
    struct funnel_format *format;

    pw_array_for_each (format, formats) {
        free(format->modifiers);
    }
    pw_array_reset(formats);
}

static void funnel_free_formats(struct pw_array *formats) {
    funnel_reset_formats(formats);
    pw_array_clear(formats);
}

void funnel_stream_clear_formats(struct funnel_stream *stream) {
    funnel_reset_formats(&stream->config.formats);
    stream->config.has_nonlinear_tiling = false;
}

static void funnel_copy_formats(struct pw_array *dst, struct pw_array *src) {
    struct funnel_format *sfmt;

    funnel_reset_formats(dst);

    pw_array_for_each (sfmt, src) {
        struct funnel_format *fmt =
            pw_array_add(dst, sizeof(struct funnel_format));
        assert(fmt);
        fmt->format = sfmt->format;
        fmt->spa_format = sfmt->spa_format;
        fmt->modifiers = calloc(sfmt->num_modifiers, sizeof(uint64_t));
        fmt->num_modifiers = sfmt->num_modifiers;
        memcpy(fmt->modifiers, sfmt->modifiers,
               sfmt->num_modifiers * sizeof(uint64_t));
    }
}

int funnel_stream_set_size(struct funnel_stream *stream, uint32_t width,
                           uint32_t height) {
    assert(stream);

    if (!width || !height)
        return -EINVAL;

    stream->config.width = width;
    stream->config.height = height;
    stream->config_pending = true;

    return 0;
}

int funnel_stream_set_mode(struct funnel_stream *stream,
                           enum funnel_mode mode) {

    switch (mode) {
    case FUNNEL_ASYNC:
    case FUNNEL_DOUBLE_BUFFERED:
        stream->config.buffers.def = 5;
        stream->config.buffers.min = 4;
        stream->config.buffers.max = 8;
        break;
    case FUNNEL_SINGLE_BUFFERED:
    case FUNNEL_SYNCHRONOUS:
        stream->config.buffers.def = 4;
        stream->config.buffers.min = 3;
        stream->config.buffers.max = 8;
        break;
    default:
        return -EINVAL;
    }

    stream->config.mode = mode;
    stream->config_pending = true;

    return 0;
}

int funnel_stream_set_sync(struct funnel_stream *stream,
                           enum funnel_sync sync) {
    if (sync == FUNNEL_SYNC_EXPLICIT_ONLY)
        return -EOPNOTSUPP; // TODO

    switch (sync) {
    case FUNNEL_SYNC_EITHER:
        // It is legal to request this if the API does not support
        // explicit sync (EGL without the right extension). In that
        // case, it is converted to IMPLICIT.
        if (!stream->api_supports_explicit_sync)
            sync = FUNNEL_SYNC_IMPLICIT;
        // Fallthrough
    case FUNNEL_SYNC_IMPLICIT:
        if (stream->api_requires_explicit_sync)
            return -EOPNOTSUPP;
        break;
    case FUNNEL_SYNC_EXPLICIT_HYBRID:
    case FUNNEL_SYNC_EXPLICIT_ONLY:
        if (!stream->api_supports_explicit_sync)
            return -EOPNOTSUPP;
        break;
    }

    stream->config.sync = sync;
    stream->config_pending = true;
    return 0;
}

int funnel_stream_set_rate(struct funnel_stream *stream,
                           struct funnel_fraction def,
                           struct funnel_fraction min,
                           struct funnel_fraction max) {
    if (!def.den || !min.den || !max.den)
        return -EINVAL;

    stream->config.rate.def = def;
    stream->config.rate.min = min;
    stream->config.rate.max = max;
    stream->config_pending = true;

    return 0;
}

int funnel_stream_get_rate(struct funnel_stream *stream,
                           struct funnel_fraction *rate) {
    struct funnel_ctx *ctx = stream->ctx;
    pw_thread_loop_lock(ctx->loop);

    if (!stream->cur.ready) {
        *rate = FUNNEL_FRACTION(0, 0);
        UNLOCK_RETURN(-EINPROGRESS);
    }

    rate->num = stream->cur.video_format.framerate.num;
    rate->den = stream->cur.video_format.framerate.denom;

    UNLOCK_RETURN(0);
}

int funnel_stream_configure(struct funnel_stream *stream) {
    struct funnel_ctx *ctx = stream->ctx;

    if (!stream->config_pending)
        return 0;

    if (stream->api == API_UNSET) {
        pw_log_error("funnel_stream_set_size() must be called before "
                     "funnel_stream_configure()");
    }

    if (!stream->config.width || !stream->config.width) {
        pw_log_error("funnel_stream_set_size() must be called before "
                     "funnel_stream_configure()");
        return -EINVAL;
    }

    size_t num_formats =
        pw_array_get_len(&stream->config.formats, struct funnel_format);

    if (!num_formats) {
        pw_log_error("no formats configured");
        return -EINVAL;
    }

    pw_thread_loop_lock(ctx->loop);

    if (ctx->dead)
        UNLOCK_RETURN(-EIO);

    const char *driver_prio = NULL;
    bool lazy = false, request = false;
    switch (stream->config.mode) {
    case FUNNEL_ASYNC:
        driver_prio = "1";
        request = true;
        break;
    case FUNNEL_DOUBLE_BUFFERED:
    case FUNNEL_SINGLE_BUFFERED:
    case FUNNEL_SYNCHRONOUS:
        lazy = true;
        break;
    }

    bool new_stream = false;
    if (!stream->stream) {
        new_stream = true;

        struct pw_properties *props;
        // clang-format off
        props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CLASS, "Stream/Output/Video",
            PW_KEY_NODE_SUPPORTS_LAZY, lazy ? "1" : NULL,
            PW_KEY_NODE_SUPPORTS_REQUEST, request ? "1" : NULL,
            PW_KEY_PRIORITY_DRIVER, driver_prio,
            NULL
        );
        // clang-format on
        assert(props);

        stream->stream = pw_stream_new(ctx->core, stream->name, props);
        assert(stream->stream);

        pw_stream_add_listener(stream->stream, &stream->stream_listener,
                               &stream_events, stream);
    } else {
        struct pw_properties *props;

        // clang-format off
        props = pw_properties_new(
            PW_KEY_NODE_SUPPORTS_LAZY, lazy ? "1" : NULL,
            PW_KEY_NODE_SUPPORTS_REQUEST, request ? "1" : NULL,
            PW_KEY_PRIORITY_DRIVER, driver_prio,
            NULL
        );
        // clang-format on
        assert(props);

        pw_stream_update_properties(stream->stream, &props->dict);
        pw_properties_free(props);
    }

    funnel_free_formats(&stream->cur.config.formats);
    stream->cur.config = stream->config;
    pw_array_init(&stream->cur.config.formats, 32);
    funnel_copy_formats(&stream->cur.config.formats, &stream->config.formats);

    enum pw_stream_flags flags =
        PW_STREAM_FLAG_ALLOC_BUFFERS | PW_STREAM_FLAG_DRIVER;

    const struct spa_pod **params =
        calloc(num_formats, sizeof(struct spa_pod *));

    int num_params = build_formats(stream, false, params);
    assert(num_params <= num_formats);

    if (!new_stream) {
        stream->cur.ready = false;
        pw_stream_update_params(stream->stream, params, num_params);
    } else if (pw_stream_connect(stream->stream, PW_DIRECTION_OUTPUT,
                                 SPA_ID_INVALID, flags, params,
                                 num_params) != 0) {
        free_params(params, num_params);
        pw_log_error("failed to connect to stream");
        pw_stream_destroy(stream->stream);
        stream->stream = NULL;
        UNLOCK_RETURN(-EIO);
    }

    free_params(params, num_params);

    update_timeouts(stream);

    stream->config_pending = false;

    UNLOCK_RETURN(0);
}

int funnel_buffer_get_gbm_bo(struct funnel_buffer *buf, struct gbm_bo **bo) {
    assert(buf->bo);
    *bo = buf->bo;
    return 0;
}

int funnel_stream_start(struct funnel_stream *stream) {
    int ret = funnel_stream_configure(stream);
    if (ret)
        return ret;

    assert(stream->stream);

    struct funnel_ctx *ctx = stream->ctx;
    pw_thread_loop_lock(ctx->loop);

    if (ctx->dead)
        UNLOCK_RETURN(-EIO);

    stream->active = true;
    UNLOCK_RETURN(pw_stream_set_active(stream->stream, true));
}

int funnel_stream_stop(struct funnel_stream *stream) {
    if (!stream->stream)
        return -EINVAL;

    struct funnel_ctx *ctx = stream->ctx;
    pw_thread_loop_lock(ctx->loop);

    if (ctx->dead)
        UNLOCK_RETURN(-EIO);

    // Unblock the process call if blocked
    stream->active = false;
    unblock_process_thread(stream);

    UNLOCK_RETURN(pw_stream_set_active(stream->stream, false));
}

void funnel_stream_destroy(struct funnel_stream *stream) {
    if (!stream)
        return;

    funnel_stream_stop(stream);

    struct funnel_ctx *ctx = stream->ctx;
    pw_thread_loop_lock(ctx->loop);

    funnel_free_formats(&stream->config.formats);
    funnel_free_formats(&stream->cur.config.formats);

    if (stream->stream) {
        pw_stream_disconnect(stream->stream);
        pw_stream_destroy(stream->stream);
    }

    if (stream->timer) {
        pw_loop_destroy_source(pw_thread_loop_get_loop(stream->ctx->loop),
                               stream->timer);
    }

    if (stream->funcs && stream->funcs->destroy)
        stream->funcs->destroy(stream);

    pw_thread_loop_unlock(ctx->loop);

    if (stream->dummy_syncobj) {
        int fd = gbm_device_get_fd(stream->gbm);
        int ret = drmSyncobjDestroy(fd, stream->dummy_syncobj);
        assert(ret == 0);
    }

    if (stream->gbm) {
        int fd = gbm_device_get_fd(stream->gbm);
        gbm_device_destroy(stream->gbm);
        close(fd);
    }

    free((void *)stream->name);
    free(stream);
}

int funnel_stream_dequeue(struct funnel_stream *stream,
                          struct funnel_buffer **pbuf) {
    if (!stream->stream)
        return -EINVAL;

    *pbuf = NULL;
    struct funnel_ctx *ctx = stream->ctx;
    pw_thread_loop_lock(ctx->loop);

    if (stream->buffers_dequeued > 0) {
        pw_log_error("libfunnel: Dequeueing multiple buffers not supported");
        UNLOCK_RETURN(-EINVAL);
    }

    enum pw_stream_state state;
    struct pw_buffer *pwbuffer;

    for (pwbuffer = NULL;; pw_thread_loop_wait(ctx->loop)) {
        if (ctx->dead)
            UNLOCK_RETURN(-EIO);

        if (!stream->active)
            UNLOCK_RETURN(-ESHUTDOWN);

        if (stream->skip_frames) {
            stream->skip_frames--;
            UNLOCK_RETURN(0);
        }

        state = pw_stream_get_state(stream->stream, NULL);
        if (state != PW_STREAM_STATE_STREAMING) {
            if (stream->cur.config.mode == FUNNEL_ASYNC)
                UNLOCK_RETURN(0);
            pw_log_info("dequeue: Wait for stream start");
            unblock_process_thread(stream);
            continue;
        }

        if (stream->cur.config.mode == FUNNEL_SINGLE_BUFFERED &&
            is_buffer_pending(stream)) {
            pw_log_trace("dequeue: 1B, waiting for pending frame");
            unblock_process_thread(stream);
            continue;
        }

        if (stream->cur.config.mode == FUNNEL_SYNCHRONOUS &&
            stream->cycle_state != SYNC_CYCLE_ACTIVE) {
            /*
             * Tell the process callback that we are ready to start processing
             * a frame.
             */
            pw_log_trace("## Wait for process (sync)");
            stream->cycle_state = SYNC_CYCLE_WAITING;
            continue;
        }

        pw_log_trace("Try dequeue");
        int retries = stream->num_buffers;

        assert(stream->num_buffers > 0);

        /*
         * Work around PipeWire weirdness with in-use buffers
         * by trying to dequeue every possible buffer until we
         * find one that is not in use.
         */
        do {
            pwbuffer = pw_stream_dequeue_buffer(stream->stream);
        } while (!pwbuffer && errno == EBUSY && --retries);

        if (pwbuffer)
            break;

        pw_log_warn("dequeue: out of buffers?");
        if (stream->cur.config.mode == FUNNEL_ASYNC)
            UNLOCK_RETURN(0);
    }

    struct funnel_buffer *buf = pwbuffer->user_data;
    pw_log_trace("  Dequeue buffer %p (%p)", pwbuffer, buf);

    assert(!buf->dequeued);
    stream->buffers_dequeued++;
    buf->dequeued = true;

    buf->acquire.queried = false;
    buf->release.queried = false;
    buf->release_sync_file_set = false;

    *pbuf = buf;

    UNLOCK_RETURN(0);
}
static int funnel_stream_enqueue_internal(struct funnel_stream *stream,
                                          struct funnel_buffer *buf,
                                          bool valid) {
    struct funnel_ctx *ctx = stream->ctx;
    pw_thread_loop_lock(ctx->loop);

    assert(stream->buffers_dequeued > 0);
    assert(buf->dequeued);
    buf->dequeued = false;
    stream->buffers_dequeued--;

    while (1) {
        if (!buf->pw_buffer) {
            funnel_buffer_free(buf);
            unblock_process_thread(stream);
            UNLOCK_RETURN(-ESTALE);
        }

        if (ctx->dead || !stream->active) {
            pw_stream_return_buffer(stream->stream, buf->pw_buffer);
            UNLOCK_RETURN(ctx->dead ? -EIO : -ESHUTDOWN);
        }

        enum pw_stream_state state = pw_stream_get_state(stream->stream, NULL);
        if (state != PW_STREAM_STATE_STREAMING) {
            pw_stream_return_buffer(stream->stream, buf->pw_buffer);
            unblock_process_thread(stream);
            UNLOCK_RETURN(-EAGAIN);
        }

        if (stream->cur.config.mode == FUNNEL_ASYNC) {
            assert(valid); // ASYNC handled in funnel_stream_return
            if (stream->pending_buffer)
                return_buffer(stream, stream->pending_buffer);
            stream->pending_buffer = NULL;
        } else if (is_buffer_pending(stream)) {
            unblock_process_thread(stream);
            pw_thread_loop_wait(ctx->loop);
            continue;
        }
        break;
    }

    if (stream->cur.config.mode == FUNNEL_SYNCHRONOUS &&
        stream->cycle_state != SYNC_CYCLE_ACTIVE) {
        pw_log_info("enqueue: Aborted sync cycle, dropping buffer");
        UNLOCK_RETURN(-ESTALE);
    }

    assert(!is_buffer_pending(stream));
    if (valid) {
        stream->pending_buffer = buf;
    } else {
        stream->skip_buffer = true;
        return_buffer(stream, buf);
    }
    unblock_process_thread(stream);

    if (stream->cur.config.mode == FUNNEL_ASYNC)
        pw_stream_trigger_process(stream->stream);

    UNLOCK_RETURN(0);
}

int funnel_stream_enqueue(struct funnel_stream *stream,
                          struct funnel_buffer *buf) {
    if (!stream->stream)
        return -EINVAL;
    if (!buf)
        return -EINVAL;
    assert(buf->stream == stream);

    int ret;

    if (buf->frontend_sync) {
        if (!buf->release.queried || !buf->acquire.queried) {
            pw_log_error("Attempted to enqueue buffer without sync, but "
                         "sync is in use");
            return -EINVAL;
        }
    }

    if (stream->funcs && stream->funcs->enqueue_buffer) {
        ret = stream->funcs->enqueue_buffer(buf);
        if (ret < 0)
            return ret;
    }

    if (buf->frontend_sync) {
        if (!buf->backend_sync && !buf->release_sync_file_set) {
            int fd = -1;

            ret = funnel_stream_export_sync_file(
                buf->stream, buf->release.handle, buf->release.point, &fd);
            if (ret) {
                pw_log_error(
                    "Failed to export sync, did you commmit the timeline "
                    "point? (handle = %d, point=%lld)",
                    buf->release.handle, (long long)buf->release.point);
                return -errno;
            }
            assert(fd >= 0);

            buf->release.point++;

            struct dma_buf_import_sync_file args = {
                .flags = DMA_BUF_SYNC_WRITE,
                .fd = fd,
            };

            ret = drmIoctl(buf->fds[0], DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &args);
            assert(ret >= 0);
            close(args.fd);
        }
    }

    return funnel_stream_enqueue_internal(stream, buf, true);
}

int funnel_stream_return(struct funnel_stream *stream,
                         struct funnel_buffer *buf) {
    if (!stream->stream)
        return -EINVAL;
    if (!buf)
        return -EINVAL;
    assert(buf->stream == stream);

    struct funnel_ctx *ctx = stream->ctx;
    pw_thread_loop_lock(ctx->loop);

    if (stream->cur.config.mode == FUNNEL_ASYNC) {
        assert(stream->buffers_dequeued > 0);
        assert(buf->dequeued);
        buf->dequeued = false;
        stream->buffers_dequeued--;

        unblock_process_thread(stream);

        pw_stream_trigger_process(stream->stream);

        UNLOCK_RETURN(return_buffer(stream, buf));
    } else {
        UNLOCK_RETURN(funnel_stream_enqueue_internal(stream, buf, false));
    }
}

int funnel_stream_skip_frame(struct funnel_stream *stream) {
    if (!stream->stream)
        return -EINVAL;

    struct funnel_ctx *ctx = stream->ctx;
    pw_thread_loop_lock(ctx->loop);

    stream->skip_frames++;
    pw_thread_loop_signal(ctx->loop, false);

    UNLOCK_RETURN(0);
}

void funnel_buffer_get_size(struct funnel_buffer *buf, uint32_t *width,
                            uint32_t *height) {
    *width = buf->width;
    *height = buf->height;
}

void funnel_buffer_set_user_data(struct funnel_buffer *buf, void *opaque) {
    buf->opaque = opaque;
}

void *funnel_buffer_get_user_data(struct funnel_buffer *buf) {
    return buf->opaque;
}

bool funnel_buffer_has_sync(struct funnel_buffer *buf) {
    return buf->frontend_sync;
}

int funnel_buffer_get_acquire_sync_object(struct funnel_buffer *buf,
                                          uint32_t *handle, uint64_t *point) {
    if (!buf->frontend_sync)
        return -EINVAL;

    *handle = buf->acquire.handle;
    *point = buf->acquire.point;

    if (!buf->backend_sync) {
        struct dma_buf_export_sync_file args = {
            .flags = DMA_BUF_SYNC_RW,
            .fd = -1,
        };

        int ret = drmIoctl(buf->fds[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &args);
        assert(ret >= 0);

        ret = funnel_stream_import_sync_file(buf->stream, buf->acquire.handle,
                                             args.fd, buf->acquire.point);
        assert(ret >= 0);
        buf->acquire.point++;
        close(args.fd);
    }

    buf->acquire.queried = true;
    return 0;
}

int funnel_buffer_get_release_sync_object(struct funnel_buffer *buf,
                                          uint32_t *handle, uint64_t *point) {
    if (!buf->frontend_sync)
        return -EINVAL;

    if (buf->release_sync_file_set) {
        pw_log_error("Cannot mix sync file and sync object APIs");
        return -EINVAL;
    }

    *handle = buf->release.handle;
    *point = buf->release.point;

    buf->release.queried = true;
    return 0;
}

int funnel_buffer_get_acquire_sync_file(struct funnel_buffer *buf, int *fd) {
    *fd = -1;

    if (!buf->backend_sync) {
        struct dma_buf_export_sync_file args = {
            .flags = DMA_BUF_SYNC_RW,
            .fd = -1,
        };

        int ret = drmIoctl(buf->fds[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &args);
        assert(ret >= 0);

        *fd = args.fd;
    } else {
        int gbm_fd = gbm_device_get_fd(buf->stream->gbm);
        int ret = drmSyncobjTimelineWait(
            gbm_fd, &buf->acquire.handle, &buf->acquire.point, 1, INT64_MAX,
            DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE, NULL);
        if (ret < 0)
            return -errno;

        ret = funnel_stream_export_sync_file(buf->stream, buf->acquire.handle,
                                             buf->acquire.point, fd);
        if (ret < 0)
            return ret;
    }

    buf->acquire.queried = true;
    return 0;
}

int funnel_buffer_set_release_sync_file(struct funnel_buffer *buf, int fd) {
    if (!buf->frontend_sync)
        return -EINVAL;

    if (!buf->release_sync_file_set && buf->release.queried) {
        pw_log_error("Cannot mix sync file and sync object APIs");
        return -EINVAL;
    }

    if (!buf->backend_sync) {
        struct dma_buf_import_sync_file args = {
            .flags = DMA_BUF_SYNC_WRITE,
            .fd = fd,
        };

        int ret = drmIoctl(buf->fds[0], DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &args);
        assert(ret >= 0);
    } else {
        int ret = funnel_stream_import_sync_file(
            buf->stream, buf->release.handle, fd, buf->release.point);
        if (ret < 0)
            return ret;
    }

    buf->release_sync_file_set = true;
    buf->release.queried = true;
    return 0;
}

static int funnel_stream_import_sync_file(struct funnel_stream *stream,
                                          uint32_t handle, int fd,
                                          uint64_t point) {
    struct drm_syncobj_handle args = {
        .flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE,
        .fd = fd,
    };

    int ret;
    int gbm_fd = gbm_device_get_fd(stream->gbm);

    if (fd < 0 || !handle)
        return -EINVAL;

#ifdef DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE
    if (stream->gbm_timeline_sync_import_export) {
        args.flags |= DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE;
        args.handle = handle;
        args.point = point;
    } else
#endif
    {
        assert(stream->dummy_syncobj);
        args.handle = stream->dummy_syncobj;
    }

    ret = drmIoctl(gbm_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &args);
    if (ret < 0)
        return -errno;

    if (!stream->gbm_timeline_sync_import_export) {
        ret = drmSyncobjTransfer(gbm_fd, handle, point, stream->dummy_syncobj,
                                 0, 0);
        if (ret < 0)
            return -errno;
    }

    return 0;
}

static int funnel_stream_export_sync_file(struct funnel_stream *stream,
                                          uint32_t handle, uint64_t point,
                                          int *fd) {
    struct drm_syncobj_handle args = {
        .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE,
    };

    int ret;
    int gbm_fd = gbm_device_get_fd(stream->gbm);

    if (!fd || !handle)
        return -EINVAL;

    *fd = -1;

#ifdef DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE
    if (stream->gbm_timeline_sync_import_export) {
        args.flags |= DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE;
        args.handle = handle;
        args.point = point;
    } else
#endif
    {
        assert(stream->dummy_syncobj);
        ret = drmSyncobjTransfer(gbm_fd, stream->dummy_syncobj, 0, handle,
                                 point, 0);
        if (ret < 0)
            return -errno;

        args.handle = stream->dummy_syncobj;
    }

    ret = drmIoctl(gbm_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &args);
    if (ret < 0)
        return -errno;

    *fd = args.fd;
    return 0;
}

bool funnel_buffer_is_efficient_for_rendering(struct funnel_buffer *buf) {
    return !(buf->stream->cur.config.has_nonlinear_tiling &&
             gbm_bo_get_modifier(buf->bo) == DRM_FORMAT_MOD_LINEAR);
}
