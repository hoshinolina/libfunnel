# libfunnel Documentation {#mainpage}

libfunnel is a library to make creating PipeWire video streams easy, using zero-copy DMA-BUF frame sharing. "Spout2 / Syphon, but for Linux".

@section Lifetimes

This documentation uses the following terms to describe the lifetime of API objects:

For input arguments (the default if not specified):

* @b borrowed: The object is owned by the caller and borrowed by the function call. After the call, the caller remains responsible for releasing the object at some point.
* @b borrowed-by @c user : The object is owned by the caller and borrowed by libfunnel object @c user. The caller must release @c user before it releases this object.
* @b owned: The object ownership is transferred to libfunnel. After the call, the caller must no longer use nor release the object.

For output arguments:

* @b borrowed-from @c parent : The object is owned by the parent object @c parent. Once ownership of @c parent is transferred back to libfunnel, the borrowed object becomes invalid and may no longer be used.
* @b owned: The object ownership is transferred to the caller. After the call, the caller must eventually release the object.
* @b owned-from: The object ownership is transferred to the caller, but it is a child object of @c parent. The object must be released by the caller before @c parent is released.

@section Synchronization

libfunnel does not use thread-local state. This means that you are free to call libfunnel functions for multiple threads, as long as the calls meet the synchronization requirements. The baseline synchronization (thread safety) requirements for libfunnel objects follow from the lifetime definitions:

* An object may only be passed to a function as @owned after all calls that receive it as @borrowed complete
* An object may only be released after all borrows cease and all child objects are released

If you perform these operations in different threads, you must make sure that not only they do not overlap, but that appropriate memory sychrnonization is observed (memory barriers, etc.). If you are using standard OS synchronization mechanisms like mutexes, you do not need to worry about this, as those APIs take care of it for you.

In addition, libfunnel functions are assigned these two synchronization categories:

* @b external: The function must not be called concurrently with other functions also marked @b external borrowing the same object (typically the first parameter)
* @b internal: The function may be called concurrently with other functions (@b external and @b internal) borrowing the same object (typically the first parameter)

Note that these requirements do not override the lifetime requirements. For example, you cannot destroy an object concurrently with a call to a function that borrows it, even if the latter call is marked with @b internal synchronization.

### Usage notes

The general design of libfunnel synchronization is roughly that:

* Streams may be created and managed by independent threads
* Each unique stream must be *configured* by a single thread (or with external locking)
* Stream data processing (dequeing/enqueuing buffers) may happen in a different thread (or multiple threads, in principle)
* Stream status (start/stop/skip frame) may also be managed by arbitrary threads

Internally, libfunnel uses a single PipeWire thread loop per funnel_ctx, and synchronization happens using a context-global lock. Therefore, if your application has multiple *completely independent* streams that have no relation to each other and are managed by different threads, it may be more efficient to create a whole new funnel_ctx for each thread, and therefore have independent PipeWire daemon connections and thread loops. This is particularly relevant if you are using FUNNEL_SYNCHRONOUS mode, since in that mode the PipeWire processing thread is completely blocked while any stream has a buffer dequeued.

## API documentation

- @ref funnel.h "<funnel/funnel.h>" The core API used by all backends
- @ref funnel-gbm.h "<funnel/funnel-gbm.h>" Raw GBM buffer API
- @ref funnel-vk.h "<funnel/funnel-vk.h>" Vulkan API integration
- @ref funnel-egl.h "<funnel/funnel-egl.h>" EGL API integration
