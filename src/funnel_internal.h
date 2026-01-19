#pragma once

#include "funnel.h"
#include <gbm.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/raw-utils.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

#define UNLOCK_RETURN(ret)                                                     \
    do {                                                                       \
        int _ret = ret;                                                        \
        pw_thread_loop_unlock(ctx->loop);                                      \
        return _ret;                                                           \
    } while (0)

static inline struct spa_fraction to_spa_fraction(struct funnel_fraction frac) {
    return SPA_FRACTION(frac.num, frac.den);
}

struct funnel_ctx {
    bool dead;
    struct pw_thread_loop *loop;
    struct pw_core *core;
    struct pw_context *context;
    struct spa_hook core_listener;
};

struct funnel_format {
    uint32_t format;
    enum spa_video_format spa_format;
    uint64_t *modifiers;
    size_t num_modifiers;
};

struct funnel_stream_config {
    enum funnel_mode mode;
    enum funnel_sync sync;

    struct {
        int def, min, max;
    } buffers;

    struct {
        struct funnel_fraction def, min, max;
    } rate;

    uint32_t width;
    uint32_t height;

    struct pw_array formats;
    bool has_nonlinear_tiling;
};

enum funnel_api {
    API_UNSET = 0,
    API_GBM,
    API_EGL,
    API_VULKAN,
};

struct funnel_stream_funcs {
    void (*alloc_buffer)(struct funnel_buffer *);
    void (*free_buffer)(struct funnel_buffer *);
    int (*enqueue_buffer)(struct funnel_buffer *);
    void (*destroy)(struct funnel_stream *);
};

enum funnel_sync_cycle {
    SYNC_CYCLE_INACTIVE,
    SYNC_CYCLE_WAITING,
    SYNC_CYCLE_ACTIVE,
};

struct funnel_sync_point {
    uint32_t handle;
    uint64_t point;
    bool queried;
};

struct funnel_stream {
    struct funnel_ctx *ctx;
    const char *name;
    enum funnel_api api;
    funnel_buffer_callback alloc_cb;
    funnel_buffer_callback free_cb;
    void *cb_opaque;

    const struct funnel_stream_funcs *funcs;
    void *api_ctx;
    bool api_supports_explicit_sync;
    bool api_requires_explicit_sync;

    struct gbm_device *gbm;
    bool gbm_timeline_sync;
    bool gbm_timeline_sync_import_export;
    uint32_t dummy_syncobj;

    struct spa_hook stream_listener;
    struct pw_stream *stream;
    struct spa_source *timer;

    struct funnel_stream_config config;
    bool config_pending;

    uint32_t cur_format;
    uint64_t cur_modifier;

    bool active;
    int num_buffers;
    enum funnel_sync_cycle cycle_state;
    int buffers_dequeued;
    struct funnel_buffer *pending_buffer;
    bool skip_buffer;

    struct {
        struct funnel_stream_config config;
        bool ready;
        struct spa_video_info_raw video_format;

        struct funnel_fraction rate;
        uint32_t plane_count;
        uint32_t width;
        uint32_t height;
        uint32_t format;
        uint64_t modifier;
        uint32_t strides[4];
        uint32_t offsets[4];
    } cur;
};

struct funnel_buffer {
    struct funnel_stream *stream;
    struct pw_buffer *pw_buffer;
    bool dequeued;
    bool driving;
    struct gbm_bo *bo;
    int fds[6];
    void *api_buf;
    void *opaque;

    bool backend_sync;
    bool frontend_sync;
    struct funnel_sync_point acquire;
    struct funnel_sync_point release;
    bool release_sync_file_set;
};
