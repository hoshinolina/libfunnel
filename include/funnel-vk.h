#pragma once

#include "funnel.h"
#include <vulkan/vulkan.h>

/** @file
 * libfunnel Vulkan API integration
 *
 * ## Required Vulkan device extensions
 *
 * ### For Vulkan 1.2+:
 * - VK_KHR_external_semaphore_fd
 * - VK_KHR_external_memory_fd
 * - VK_EXT_external_memory_dma_buf
 * - VK_EXT_image_drm_format_modifier
 *
 * ### In addition, for Vulkan 1.1:
 * - VK_KHR_image_format_list
 *
 * ### In addition, for Vulkan 1.0:
 * - VK_KHR_external_memory
 * - VK_KHR_maintenance1
 * - VK_KHR_bind_memory2
 * - VK_KHR_sampler_ycbcr_conversion
 * - VK_KHR_get_memory_requirements2
 * - VK_KHR_external_semaphore
 *
 * ## Required Vulkan device extensions
 *
 * ### For Vulkan 1.1+:
 * No extensions required.
 *
 * ### In addition, for Vulkan 1.0:
 * - VK_KHR_get_physical_device_properties2
 * - VK_KHR_external_memory_capabilities
 * - VK_KHR_external_semaphore_capabilities
 */

/**
 * Set up a stream for Vulkan integration.
 *
 * @sync-ext
 *
 * @param stream Stream @borrowed
 * @param instance VkInstance to use for the stream @borrowed-by{stream}
 * @param physical_device VkPhysicalDevice to use for the stream
 *                        @borrowed-by{stream}
 * @param device VkDevice to use for the stream @borrowed-by{stream}
 * @return_err
 * @retval -EEXIST The API was already initialized once
 * @retval -ENOTSUP Missing Vulkan extensions
 * @retval -ENODEV
 *  * Could not locate DRM render node
 *  * GBM or Vulkan initialization failed
 */
int funnel_stream_init_vulkan(struct funnel_stream *stream, VkInstance instance,
                              VkPhysicalDevice physical_device,
                              VkDevice device);

/**
 * Set the required buffer usage. This will control the usage for
 * images allocated by libfunnel.
 *
 * funnel_stream_vk_add_format() will fail if the requested usages
 * are not available. In this case, you may reconfigure the usage
 * and try again.
 *
 * @sync-ext
 *
 * @param stream Stream @borrowed
 * @param usage Required VkImageUsageFlagBits.
 * @return_err
 * @retval -EINVAL
 *  * Invalid argument
 *  * API is not Vulkan
 */
int funnel_stream_vk_set_usage(struct funnel_stream *stream,
                               VkImageUsageFlagBits usage);

/**
 * Add a supported Vulkan format. Must be called in preference order (highest to
 * lowest). Only some formats are supported by libfunnel:
 *
 * - VK_FORMAT_R8G8B8A8_SRGB
 * - VK_FORMAT_R8G8B8A8_UNORM
 * - VK_FORMAT_B8G8R8A8_SRGB
 * - VK_FORMAT_B8G8R8A8_UNORM
 *
 * The corresponding UNORM variants are also acceptable, and equivalent.
 * `funnel_buffer_get_vk_format` will always return the SRGB formats. If
 * you need UNORM (because you are doing sRGB/gamma conversion in your shader),
 * you can use UNORM constants when you create a VkImageView.
 *
 * @sync-ext
 *
 * @param stream Stream @borrowed
 * @param format VkFormat
 * @param alpha Whether alpha is meaningful or ignored
 * @param features Required VkFormatFeatureFlagBits. Adding a format will fail
 * if the requested features are not available.
 * @return_err
 * @retval -EINVAL
 *  * Invalid argument
 *  * API is not Vulkan
 * @retval -ENOTSUP VkFormat is not supported by libfunnel
 * @retval -ENOENT VkFormat is not supported by the device or not usable
 */
int funnel_stream_vk_add_format(struct funnel_stream *stream, VkFormat format,
                                bool alpha, VkFormatFeatureFlagBits features);

/**
 * Get the VkImage for a Funnel buffer.
 *
 * The VkImage is only valid while `buf` is dequeued, or before the destroy
 * callback is used (if you use buffer callbacks).
 *
 * @sync-ext
 *
 * @param buf Buffer @borrowed
 * @param[out] pimage VkImage for the buffer @borrowed-from{buf}
 * @return_err
 * @retval -EINVAL
 *  * Invalid argument
 *  * API is not Vulkan
 */
int funnel_buffer_get_vk_image(struct funnel_buffer *buf, VkImage *pimage);

/**
 * Get the VkFormat for a Funnel buffer.
 *
 * @sync-ext
 *
 * @param buf Buffer @borrowed
 * @param[out] pformat VkFormat for the buffer
 * @param[out] phas_alpha Boolean indicating whether alpha is enabled
 * @return_err
 * @retval -EINVAL
 *  * Invalid argument
 *  * API is not Vulkan
 * @retval -EIO Format is unsupported (internal error)
 */
int funnel_buffer_get_vk_format(struct funnel_buffer *buf, VkFormat *pformat,
                                bool *phas_alpha);

/**
 * Get the VkSemaphores for acquiring and releasing the buffer.
 *
 * The user must wait on the acquier VkSemaphore object before accessing
 * the buffer, and signal the release VkSemaphore after accessing the buffer.
 * These semaphores are valid while the buffer is dequeued.
 *
 * @sync-ext
 *
 * @param buf Buffer @borrowed
 * @param[out] pacquire Acquire VkSemaphore @borrowed-from{buf}
 * @param[out] prelease Release VkSemaphore @borrowed-from{buf}
 * @return_err
 * @retval -EINVAL
 *  * Invalid argument
 *  * API is not Vulkan
 * @retval -EBUSY Already called once for this buffer
 * @retval -EIO Failed to import acquire semaphore into Vulkan
 */
int funnel_buffer_get_vk_semaphores(struct funnel_buffer *buf,
                                    VkSemaphore *pacquire,
                                    VkSemaphore *prelease);

/**
 * Get the VkFence that must be signaled by the queue batch
 *
 * The user must pass this fence to vkQueueSubmit() (or similar),
 * such that it is signaled when all operations on the buffer
 * are complete. This fence is valid whil the buffer is
 * dequeued.
 *
 * @sync-ext
 *
 * @param buf Buffer @borrowed
 * @param[out] pfence Completion VkFence @borrowed-from{buf}
 * @return_err
 * @retval -EINVAL
 *  * Invalid argument
 *  * API is not Vulkan
 * @retval -EBUSY Already called once for this buffer
 */
int funnel_buffer_get_vk_fence(struct funnel_buffer *buf, VkFence *pfence);
