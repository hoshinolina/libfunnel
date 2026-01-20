#include "funnel-gbm.h"
#include "funnel-vk.h"
#include "funnel.h"
#include "funnel_internal.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

PW_LOG_TOPIC_STATIC(log_funnel_vk, "funnel.vk");
#define PW_LOG_TOPIC_DEFAULT log_funnel_vk

struct funnel_vk_stream {
    VkInstance instance;
    VkDevice device;
    VkPhysicalDevice physical_device;
    VkImageUsageFlagBits usage;

    PFN_vkGetMemoryFdPropertiesKHR vkGetMemoryFdPropertiesKHR;
    PFN_vkGetImageMemoryRequirements2KHR vkGetImageMemoryRequirements2KHR;
    PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR;
    PFN_vkImportSemaphoreFdKHR vkImportSemaphoreFdKHR;
};

struct funnel_vk_buffer {
    VkImage image;
    VkDeviceMemory mem;
    VkSemaphore acquire;
    VkSemaphore release;
    int last_sync_file;
};

static uint32_t format_vk_to_gbm(VkFormat format, bool alpha) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        if (alpha)
            return GBM_FORMAT_ABGR8888;
        else
            return GBM_FORMAT_XBGR8888;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        if (alpha)
            return GBM_FORMAT_ARGB8888;
        else
            return GBM_FORMAT_XRGB8888;

    default:
        return 0;
    }
}

void funnel_vk_destroy(struct funnel_stream *stream) {
    free(stream->api_ctx);
    stream->api_ctx = NULL;
}

static int get_modifiers(VkPhysicalDevice physical_device, VkFormat format,
                         uint32_t *count,
                         VkDrmFormatModifierPropertiesEXT *modifiers) {
    VkDrmFormatModifierPropertiesListEXT modifier_props = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
        .drmFormatModifierCount = *count,
        .pDrmFormatModifierProperties = modifiers};

    VkFormatProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &modifier_props,
    };

    vkGetPhysicalDeviceFormatProperties2(physical_device, format, &props);

    if (!props.formatProperties.linearTilingFeatures &&
        !props.formatProperties.optimalTilingFeatures)
        return -ENOENT;

    *count = modifier_props.drmFormatModifierCount;
    return 0;
}

int funnel_stream_vk_add_format(struct funnel_stream *stream, VkFormat format,
                                bool alpha, VkFormatFeatureFlagBits features) {
    if (stream->api != API_VULKAN)
        return -EINVAL;

    struct funnel_vk_stream *vks = stream->api_ctx;

    uint32_t gbm_format = format_vk_to_gbm(format, alpha);
    if (!gbm_format)
        return -EINVAL;

    uint32_t count;
    if (get_modifiers(vks->physical_device, format, &count, NULL) < 0)
        return -ENOENT;

    VkDrmFormatModifierPropertiesEXT *modifier_props =
        malloc(sizeof(VkDrmFormatModifierPropertiesEXT) * count);
    uint64_t *modifiers = malloc(sizeof(uint64_t) * count);

    assert(get_modifiers(vks->physical_device, format, &count,
                         modifier_props) >= 0);

    pw_log_info("Check format: %d / 0x%x [%d modifiers]", format, gbm_format,
                count);

    unsigned usable = 0;
    for (unsigned i = 0; i < count; i++) {
        VkDrmFormatModifierPropertiesEXT *prop = &modifier_props[i];
        const char *unusable_reason = NULL;

        VkPhysicalDeviceImageDrmFormatModifierInfoEXT format_modifier_info = {
            .sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
            .drmFormatModifier = prop->drmFormatModifier,
            // XXX: Sharing?
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
        };

        VkExternalImageFormatProperties external_image_format_props = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
        };
        VkImageFormatProperties2 image_format_props = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
            .pNext = &external_image_format_props,
        };

        VkPhysicalDeviceExternalImageFormatInfo external_format_info = {
            .sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .pNext = &format_modifier_info,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };

        VkPhysicalDeviceImageFormatInfo2 format_info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &external_format_info,
            .format = format,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = vks->usage,
        };

        VkResult res = vkGetPhysicalDeviceImageFormatProperties2(
            vks->physical_device, &format_info, &image_format_props);

        VkExternalMemoryFeatureFlags feature_flags = 0;

        if (res != VK_SUCCESS) {
            unusable_reason = "No DMA-BUF handle support";
        } else {
            feature_flags = external_image_format_props.externalMemoryProperties
                                .externalMemoryFeatures;
            if (!(feature_flags & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
                unusable_reason = "No DMA-BUF import";
            } else if ((prop->drmFormatModifierTilingFeatures & features) !=
                       features) {
                unusable_reason = "Missing features";
            }
        }

        if (!unusable_reason) {
            modifiers[usable++] = prop->drmFormatModifier;
        }

        pw_log_info(" - 0x%llx [planes=%d, features=0x%x]: %s",
                    (long long)prop->drmFormatModifier,
                    prop->drmFormatModifierPlaneCount,
                    prop->drmFormatModifierTilingFeatures,
                    unusable_reason ?: "USABLE");
    }

    int ret = -ENOENT;
    if (usable) {
        pw_log_info("%d usable modifiers", usable);
        ret =
            funnel_stream_gbm_add_format(stream, gbm_format, modifiers, usable);
    }

    free(modifiers);
    free(modifier_props);

    return ret;
}

void funnel_vk_alloc_buffer(struct funnel_buffer *buffer) {
    struct funnel_stream *stream = buffer->stream;
    struct funnel_vk_stream *vks = stream->api_ctx;

    VkImage image;
    VkResult res;
    VkFormat format;

    assert(funnel_buffer_get_vk_format(buffer, &format, NULL) >= 0);

    VkSubresourceLayout layouts[4];

    for (int i = 0; i < stream->cur.plane_count; ++i) {
        layouts[i].offset = stream->cur.offsets[i];
        layouts[i].size = 0;
        layouts[i].rowPitch = stream->cur.strides[i];
        layouts[i].arrayPitch = 0;
        layouts[i].depthPitch = 0;
    }

    VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {
        .sType =
            VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = stream->cur.modifier,
        .drmFormatModifierPlaneCount = stream->cur.plane_count,
        .pPlaneLayouts = layouts,
    };

    VkExternalMemoryImageCreateInfo create_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &modifier_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };

    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &create_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = (VkExtent3D){stream->cur.width, stream->cur.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = 1,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = vks->usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    res = vkCreateImage(vks->device, &info, NULL, &image);
    assert(res == VK_SUCCESS);

    const VkImageMemoryRequirementsInfo2 mem_reqs_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = image,
    };
    VkMemoryRequirements2 mem_reqs = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
    };
    vks->vkGetImageMemoryRequirements2KHR(vks->device, &mem_reqs_info,
                                          &mem_reqs);

    VkMemoryFdPropertiesKHR fd_props = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
    };
    vks->vkGetMemoryFdPropertiesKHR(
        vks->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        buffer->fds[0], &fd_props);

    const uint32_t memory_type_bits =
        fd_props.memoryTypeBits & mem_reqs.memoryRequirements.memoryTypeBits;

    if (!memory_type_bits) {
        pw_log_error("No valid memory type");
        assert(0);
    }

    const VkMemoryDedicatedAllocateInfo memory_dedicated_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = image,
    };
    const VkImportMemoryFdInfoKHR memory_fd_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &memory_dedicated_info,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = dup(buffer->fds[0]),
    };

    VkMemoryAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &memory_fd_info,
        .allocationSize = mem_reqs.memoryRequirements.size,
        // XXX pick the best memory type?
        .memoryTypeIndex = ffs(memory_type_bits) - 1,
    };

    VkDeviceMemory mem;

    res = vkAllocateMemory(vks->device, &allocate_info, NULL, &mem);
    assert(res == VK_SUCCESS);

    res = vkBindImageMemory(vks->device, image, mem, 0);
    assert(res == VK_SUCCESS);

    struct funnel_vk_buffer *vkbuf = calloc(1, sizeof(struct funnel_vk_buffer));
    vkbuf->image = image;
    vkbuf->mem = mem;
    vkbuf->last_sync_file = -1;

    VkExportSemaphoreCreateInfo export_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };

    struct VkSemaphoreCreateInfo create_acquire = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };

    struct VkSemaphoreCreateInfo create_release = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &export_info,
    };

    res =
        vkCreateSemaphore(vks->device, &create_acquire, NULL, &vkbuf->acquire);
    assert(res == VK_SUCCESS);

    res =
        vkCreateSemaphore(vks->device, &create_release, NULL, &vkbuf->release);
    assert(res == VK_SUCCESS);

    buffer->api_buf = vkbuf;
    assert(funnel_buffer_has_sync(buffer));
}

static void buffer_wait_idle(struct funnel_vk_buffer *vkbuf) {
    if (vkbuf->last_sync_file != -1) {
        struct pollfd pfd = {
            .fd = vkbuf->last_sync_file,
            .events = POLLIN,
        };

        assert(poll(&pfd, 1, -1) == 1);
        assert(pfd.revents & POLLIN);

        close(vkbuf->last_sync_file);
        vkbuf->last_sync_file = -1;
    }
}

void funnel_vk_free_buffer(struct funnel_buffer *buffer) {
    struct funnel_vk_stream *vks = buffer->stream->api_ctx;
    struct funnel_vk_buffer *vkbuf = buffer->api_buf;

    buffer_wait_idle(vkbuf);
    vkDestroySemaphore(vks->device, vkbuf->acquire, NULL);
    vkDestroySemaphore(vks->device, vkbuf->release, NULL);

    vkDestroyImage(vks->device, vkbuf->image, NULL);
    vkFreeMemory(vks->device, vkbuf->mem, NULL);
}

int funnel_vk_enqueue_buffer(struct funnel_buffer *buf) {
    struct funnel_vk_buffer *vkbuf = buf->api_buf;
    struct funnel_vk_stream *vks = buf->stream->api_ctx;

    // Checked by caller
    assert(buf->acquire.queried);
    assert(buf->release.queried);
    assert(vkbuf->last_sync_file == -1);

    // Reset for GBM layer to take over
    buf->release.queried = false;

    VkSemaphoreGetFdInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
        .semaphore = vkbuf->release,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };

    int fd;
    if (vks->vkGetSemaphoreFdKHR(vks->device, &info, &fd) != VK_SUCCESS) {
        pw_log_error("Failed to export sync file from semaphore");
        return -EIO;
    }

    int ret = funnel_buffer_set_release_sync_file(buf, fd);
    vkbuf->last_sync_file = fd;

    return ret;
}

int funnel_stream_vk_set_usage(struct funnel_stream *stream,
                               VkImageUsageFlagBits usage) {
    if (!stream || stream->api != API_VULKAN)
        return -EINVAL;

    struct funnel_vk_stream *vks = stream->api_ctx;

    vks->usage = usage;

    return 0;
}

int funnel_buffer_get_vk_image(struct funnel_buffer *buf, VkImage *image) {
    *image = NULL;
    if (!buf || buf->stream->api != API_VULKAN)
        return -EINVAL;

    struct funnel_vk_buffer *vkbuf = buf->api_buf;

    *image = vkbuf->image;
    return 0;
}

int funnel_buffer_get_vk_format(struct funnel_buffer *buf, VkFormat *format,
                                bool *has_alpha) {
    *format = VK_FORMAT_UNDEFINED;

    if (!buf || buf->stream->api != API_VULKAN)
        return -EINVAL;

    switch (gbm_bo_get_format(buf->bo)) {
    case GBM_FORMAT_ARGB8888:
        *format = VK_FORMAT_B8G8R8A8_SRGB;
        if (has_alpha)
            *has_alpha = true;
        break;
    case GBM_FORMAT_ABGR8888:
        *format = VK_FORMAT_R8G8B8A8_SRGB;
        if (has_alpha)
            *has_alpha = true;
        break;
    case GBM_FORMAT_XRGB8888:
        *format = VK_FORMAT_B8G8R8A8_SRGB;
        if (has_alpha)
            *has_alpha = false;
        break;
    case GBM_FORMAT_XBGR8888:
        *format = VK_FORMAT_R8G8B8A8_SRGB;
        if (has_alpha)
            *has_alpha = false;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static const struct funnel_stream_funcs vk_funcs = {
    .alloc_buffer = funnel_vk_alloc_buffer,
    .free_buffer = funnel_vk_free_buffer,
    .enqueue_buffer = funnel_vk_enqueue_buffer,
    .destroy = funnel_vk_destroy,
};

int funnel_stream_init_vulkan(struct funnel_stream *stream, VkInstance instance,
                              VkPhysicalDevice physical_device,
                              VkDevice device) {
    PFN_vkGetPhysicalDeviceProperties2KHR vkGetPhysicalDeviceProperties2KHR =
        (PFN_vkGetPhysicalDeviceProperties2KHR)vkGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceProperties2");

    if (!vkGetPhysicalDeviceProperties2KHR)
        vkGetPhysicalDeviceProperties2KHR =
            (PFN_vkGetPhysicalDeviceProperties2KHR)vkGetInstanceProcAddr(
                instance, "vkGetPhysicalDeviceProperties2KHR");

    PFN_vkGetImageMemoryRequirements2KHR vkGetImageMemoryRequirements2KHR =
        (PFN_vkGetImageMemoryRequirements2KHR)vkGetDeviceProcAddr(
            device, "vkGetImageMemoryRequirements2");

    if (!vkGetImageMemoryRequirements2KHR)
        vkGetImageMemoryRequirements2KHR =
            (PFN_vkGetImageMemoryRequirements2KHR)vkGetDeviceProcAddr(
                device, "vkGetImageMemoryRequirements2KHR");

    PFN_vkGetMemoryFdPropertiesKHR vkGetMemoryFdPropertiesKHR =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
            device, "vkGetMemoryFdPropertiesKHR");

    PFN_vkGetSemaphoreFdKHR vkGetSemaphoreFdKHR =
        (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(device,
                                                     "vkGetSemaphoreFdKHR");

    PFN_vkImportSemaphoreFdKHR vkImportSemaphoreFdKHR =
        (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(
            device, "vkImportSemaphoreFdKHR");

    if (!vkGetPhysicalDeviceProperties2KHR || !vkGetMemoryFdPropertiesKHR ||
        !vkGetImageMemoryRequirements2KHR || !vkGetSemaphoreFdKHR ||
        !vkImportSemaphoreFdKHR) {
        pw_log_error("Missing extensions");
        return -ENODEV;
    }

    VkPhysicalDeviceDrmPropertiesEXT drm_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};

    VkPhysicalDeviceProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &drm_props,
    };

    vkGetPhysicalDeviceProperties2KHR(physical_device, &props2);

    if (!drm_props.hasRender) {
        pw_log_error("No render node?");
        return -ENODEV;
    }

    pw_log_info("Render node %d:%d", (int)drm_props.renderMajor,
                (int)drm_props.renderMinor);

    char render_node[64];

    if (drm_props.renderMinor >= 128)
        sprintf(render_node, "/dev/dri/renderD%d", (int)drm_props.renderMinor);
    else
        sprintf(render_node, "/dev/dri/card%d", (int)drm_props.renderMinor);

    int gbm_fd = open(render_node, O_RDWR);
    if (gbm_fd < 0) {
        pw_log_error("failed to open device node %s: %d", render_node, errno);
        return -errno;
    }

    int ret = funnel_stream_init_gbm(stream, gbm_fd);
    close(gbm_fd);

    if (ret < 0)
        return ret;

    struct funnel_vk_stream *vks;
    vks = calloc(1, sizeof(*vks));

    vks->instance = instance;
    vks->device = device;
    vks->physical_device = physical_device;
    vks->usage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    vks->vkGetMemoryFdPropertiesKHR = vkGetMemoryFdPropertiesKHR;
    vks->vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2KHR;
    vks->vkGetSemaphoreFdKHR = vkGetSemaphoreFdKHR;
    vks->vkImportSemaphoreFdKHR = vkImportSemaphoreFdKHR;

    ret = funnel_stream_set_sync(stream, FUNNEL_SYNC_EXPLICIT_HYBRID);
    if (ret < 0) {
        pw_log_error("Vulkan requires explicit sync, but the driver does not "
                     "support it?");
        return ret;
    }

    stream->funcs = &vk_funcs;
    stream->api = API_VULKAN;
    stream->api_ctx = vks;
    stream->api_supports_explicit_sync = true;
    stream->api_requires_explicit_sync = true;

    return 0;
}

int funnel_buffer_get_vk_semaphores(struct funnel_buffer *buf,
                                    VkSemaphore *acquire,
                                    VkSemaphore *release) {
    if (!buf || buf->stream->api != API_VULKAN)
        return -EINVAL;

    struct funnel_vk_buffer *vkbuf = buf->api_buf;
    struct funnel_vk_stream *vks = buf->stream->api_ctx;

    // Can only be called once per buffer
    if (buf->acquire.queried)
        return -EINVAL;

    // Wait for previous use to be complete
    buffer_wait_idle(vkbuf);

    int fd;
    int ret = funnel_buffer_get_acquire_sync_file(buf, &fd);
    if (ret < 0)
        return ret;

    VkImportSemaphoreFdInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = vkbuf->acquire,
        .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
        .fd = fd,
    };

    if (vks->vkImportSemaphoreFdKHR(vks->device, &info) != VK_SUCCESS) {
        pw_log_error("Failed to import sync file into semaphore");
        return -EIO;
    }

    buf->release.queried = true;

    *acquire = vkbuf->acquire;
    *release = vkbuf->release;
    return 0;
}
