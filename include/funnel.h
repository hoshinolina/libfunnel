#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct funnel_ctx;
struct funnel_stream;
struct funnel_buffer;

/**
 * A rational frame rate
 */
struct funnel_fraction {
    uint32_t num;
    uint32_t den;
};

/**
 * Indicates that the frame rate is variable
 */
static const struct funnel_fraction FUNNEL_RATE_VARIABLE = {0, 1};

static inline struct funnel_fraction FUNNEL_FRACTION(uint32_t num,
                                                     uint32_t den) {
    return (struct funnel_fraction){num, den};
}

typedef void (*funnel_buffer_callback)(void *opaque,
                                       struct funnel_stream *stream,
                                       struct funnel_buffer *buf);

/**
 * Synchronization modes for the frame transfer
 */
enum funnel_mode {
    /**
     * Produce frames asynchronously to the consumer.
     *
     * In this mode, libfunnel calls never block and you
     * must be able to handle the lack of a buffer (by
     * skipping rendering/copying to it). This mode only
     * makes sense if your application is FPS-limited by
     * some other consumer (for example, if it renders to
     * the screen, usually with VSync). You should configure
     * the frame rate you expect to produce frames at with
     * `funnel_stream_set_rate()`.
     *
     * This mode essentially behaves like triple buffering.
     * Whenever the PipeWire cycle runs, the consumer will
     * receive the frame that was most recently submitted
     * to funnel_stream_enqueue().
     */
    FUNNEL_ASYNC,
    /**
     * Produce frames synchronously to the consumer with
     * double buffering.
     *
     * In this mode, after a frame is produced, it is
     * queued to be sent out to the consumer in the next
     * PipeWire process cycle, and you may immediately
     * dequeue a new buffer to start rendering the next
     * frame. libfunnel will block at `funnel_stream_enqueue()`
     * until the previously queued frame has been consumed.
     * In this mode, `funnel_stream_dequeue()` will only
     * block if there are no free buffers (if the consumer is
     * not freeing buffers quickly enough).
     *
     * This mode effectively adds two frames of latency,
     * as up to two frames can be rendered ahead of the
     * PipeWire cycle (one ready to be submitted, and
     * one blocked at `funnel_stream_enqueue()`).
     */
    FUNNEL_DOUBLE_BUFFERED,
    /**
     * Produce frames synchronously to the consumer with
     * single buffering.
     *
     * In this mode, after a frame is produced, it is
     * queued to be sent out to the consumer in the next
     * PipeWire process cycle. When you are ready to begin
     * rendering a new frame, libfunnel will block
     * at `funnel_stream_dequeue()` until the previous frame
     * has been sent to the consumer. In this mode,
     * `funnel_stream_enqueue()` will never block.
     *
     * This mode effectively adds one frame of latency,
     * as only one frame can be rendered ahead of the
     * PipeWire cycle.
     */
    FUNNEL_SINGLE_BUFFERED,
    /**
     * Produce frames synchronously with the PipeWire process
     * cycle.
     *
     * In this mode, `funnel_stream_dequeue()` will wait for
     * the beginning of a PipeWire process cycle, and the
     * process cycle will be blocked until the frame is
     * submitted with `funnel_stream_enqueue()`.
     *
     * This mode provides the lowest possible latency, but
     * is only suitable for applications that do not do much
     * work to render frames (for example, just a copy), as
     * the PipeWire graph will be blocked while the buffer
     * is dequeued. It adds no latency.
     */
    FUNNEL_SYNC,
};

/**
 * Create a Funnel context.
 *
 * @param ctx New context
 */
int funnel_init(struct funnel_ctx **pctx);

/**
 * Shut down a Funnel context.
 *
 * @param ctx Context
 */
void funnel_shutdown(struct funnel_ctx *ctx);

/**
 * Create a new stream.
 *
 * @param ctx Context
 * @param name Name of the new stream (borrow)
 * @param stream New stream (must not outlive context)
 */
int funnel_stream_create(struct funnel_ctx *ctx, const char *name,
                         struct funnel_stream **pstream);

/**
 * Specify callbacks for buffer creation/destruction.
 *
 * @param stream Stream
 * @param alloc Callback when a buffer is allocated
 * @param free Callback when a buffer is freed
 * @param opaque Opaque user pointer
 */
void funnel_stream_set_buffer_callbacks(struct funnel_stream *stream,
                                        funnel_buffer_callback alloc,
                                        funnel_buffer_callback free,
                                        void *opaque);

/**
 * Set the frame dimensions for a stream.
 *
 * @param stream Stream
 * @param width Width in pixels
 * @param height Height in pixels
 */
int funnel_stream_set_size(struct funnel_stream *stream, uint32_t width,
                           uint32_t height);

/**
 * Configure the queueing mode for the stream.
 *
 * @param stream Stream
 * @param mode Queueing mode for the stream
 */
int funnel_stream_set_mode(struct funnel_stream *stream, enum funnel_mode mode);

/**
 * Set the frame rate of a stream.
 *
 * @param stream Stream
 * @param def Default frame rate (FUNNEL_RATE_VARIABLE for no default or
 * variable)
 * @param min Minimum frame rate (FUNNEL_RATE_VARIABLE if variable)
 * @param max Maximum frame rate (FUNNEL_RATE_VARIABLE if variable)
 */
int funnel_stream_set_rate(struct funnel_stream *stream,
                           struct funnel_fraction def,
                           struct funnel_fraction min,
                           struct funnel_fraction max);

/**
 * Get the currently negotiated frame rate of a stream.
 *
 * @param stream Stream
 * @param rate Output frame rate
 */
int funnel_stream_get_rate(struct funnel_stream *stream,
                           struct funnel_fraction *prate);

/**
 * Clear the supported format list. Used for reconfiguration.
 *
 * @param stream Stream
 */
void funnel_stream_clear_formats(struct funnel_stream *stream);

/**
 * Apply the stream configuration and register the stream with PipeWire.
 *
 * If called on an already configured stream, this will update the
 * configuration.
 *
 * @param stream Stream
 */
int funnel_stream_configure(struct funnel_stream *stream);

/**
 * Start running a stream.
 *
 * @param stream Stream
 */
int funnel_stream_start(struct funnel_stream *stream);

/**
 * Stop running a stream.

 * This function may be called from any thread, which is
 * useful to abort a call to `funnel_stream_dequeue()`
 * in one of the synchronous modes.
 *
 * @param stream Stream
 */
int funnel_stream_stop(struct funnel_stream *stream);

/**
 * Destroy a stream.
 *
 * The stream will be stopped if it is running.
 *
 * @param stream Stream
 */
void funnel_stream_destroy(struct funnel_stream *stream);

/**
 * Dequeue a buffer from a stream.
 *
 * Note that, currently, you may only have one buffer
 * dequeued at a time.
 *
 * @param stream Stream
 * @param buf Output buffer (NULL if no buffer is available)
 */
int funnel_stream_dequeue(struct funnel_stream *stream,
                          struct funnel_buffer **pbuf);

/**
 * Enqueue a buffer to a stream.
 *
 * After this call, the buffer is no longer owned by the user and may not be
 * queued again until it is dequeued.
 *
 * @param stream Stream
 * @param buf Buffer to enqueue (must have been dequeued)
 */
int funnel_stream_enqueue(struct funnel_stream *stream,
                          struct funnel_buffer *buf);

/**
 * Return a buffer to the pool without enqueueing it.
 *
 * After this call, the buffer is no longer owned by the user and may not be
 * queued again until it is dequeued. This will effectively drop one frame.
 *
 * @param stream Stream
 * @param buf Buffer to return (must have been dequeued)
 */
int funnel_stream_return(struct funnel_stream *stream,
                         struct funnel_buffer *buf);

/**
 * Get the dimensions of a Funnel buffer.
 *
 * @param buf Buffer
 * @param pwidth Output width
 * @param pheight Output height
 */
void funnel_buffer_get_size(struct funnel_buffer *buf, uint32_t *pwidth,
                            uint32_t *pheight);

/**
 * Set an arbitrary user data pointer for a buffer.
 *
 * @param buf Buffer
 * @param opaque Opaque user data pointer
 */
void funnel_buffer_set_user_data(struct funnel_buffer *buf, void *opaque);

/**
 * Get an arbitrary user data pointer for a buffer.
 *
 * @param buf Buffer
 */
void *funnel_buffer_get_user_data(struct funnel_buffer *buf);
