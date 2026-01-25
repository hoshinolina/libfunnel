// Based on https://gitlab.com/amini-allight/wayland-vulkan-example

#include <funnel-vk.h>
#include <funnel.h>

#define API_VERSION VK_API_VERSION_1_0
// #define HAVE_VK_1_1
// #define HAVE_VK_1_2

#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <asm/errno.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_wayland.h>
#include <wayland-client.h>

#include "triangle_frag.h"
#include "triangle_vert.h"

#define CHECK_VK_RESULT(_expr)                                                 \
    result = _expr;                                                            \
    if (result != VK_SUCCESS) {                                                \
        printf("Error executing %s: %i\n", #_expr, result);                    \
    }

#define GET_EXTENSION_FUNCTION(_id)                                            \
    ((PFN_##_id)(vkGetInstanceProcAddr(instance, #_id)))

#define CHECK_WL_RESULT(_expr)                                                 \
    if (!(_expr)) {                                                            \
        printf("Error executing %s.", #_expr);                                 \
    }

struct SwapchainElement {
    VkCommandBuffer commandBuffer;
    VkImage image;
    VkImageView imageView;
    VkFramebuffer framebuffer;
    VkSemaphore startSemaphore;
    VkSemaphore endSemaphore;
    VkFence fence;
    VkFence lastFence;
};

static const char *const appName = "Wayland Vulkan Example";
static const char *const instanceExtensionNames[] = {
    "VK_EXT_debug_utils",
    "VK_KHR_surface",
    "VK_KHR_wayland_surface",

#ifndef HAVE_VK_1_1
    "VK_KHR_get_surface_capabilities2",
    "VK_EXT_surface_maintenance1",
    "VK_KHR_get_physical_device_properties2",
    "VK_KHR_external_memory_capabilities",
    "VK_KHR_external_semaphore_capabilities",
#endif
};

static const char *const deviceExtensionNames[] = {
    "VK_KHR_swapchain",

#ifndef HAVE_VK_1_1
    "VK_EXT_swapchain_maintenance1",
    "VK_KHR_external_memory",
    "VK_KHR_maintenance1",
    "VK_KHR_bind_memory2",
    "VK_KHR_sampler_ycbcr_conversion",
    "VK_KHR_get_memory_requirements2",
    "VK_KHR_external_semaphore",
#endif
#ifndef HAVE_VK_1_2
    "VK_KHR_image_format_list",
#endif

    "VK_KHR_external_semaphore_fd",
    "VK_KHR_external_memory_fd",
    "VK_EXT_external_memory_dma_buf",
    "VK_EXT_image_drm_format_modifier",
};
static const char *const layerNames[] = {"VK_LAYER_KHRONOS_validation"};
static VkInstance instance = VK_NULL_HANDLE;
static VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
static VkSurfaceKHR vulkanSurface = VK_NULL_HANDLE;
static VkPhysicalDevice physDevice = VK_NULL_HANDLE;
static VkDevice device = VK_NULL_HANDLE;
static uint32_t queueFamilyIndex = 0;
static VkQueue queue = VK_NULL_HANDLE;
static VkCommandPool commandPool = VK_NULL_HANDLE;
static VkSwapchainKHR swapchain = VK_NULL_HANDLE;
static VkRenderPass renderPass = VK_NULL_HANDLE;
static VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
static VkPipeline pipeline = VK_NULL_HANDLE;
static struct SwapchainElement *elements = NULL;
static struct wl_display *display = NULL;
static struct wl_registry *registry = NULL;
static struct wl_compositor *compositor = NULL;
static struct wl_surface *surface = NULL;
static struct xdg_wm_base *shell = NULL;
static struct xdg_surface *shellSurface = NULL;
static struct xdg_toplevel *toplevel = NULL;
static struct zxdg_decoration_manager_v1 *decoration_manager = NULL;
static struct zxdg_toplevel_decoration_v1 *decoration;
static int quit = 0;
static int readyToResize = 0;
static int resize = 0;
static int newWidth = 0;
static int newHeight = 0;
static VkFormat format = VK_FORMAT_UNDEFINED;
static uint32_t width = 512;
static uint32_t height = 512;
static uint32_t currentFrame = 0;
static uint32_t imageIndex = 0;
static uint32_t imageCount = 0;

struct PushConstants {
    float frame;
};

VkShaderModule vertexShaderModule = VK_NULL_HANDLE;
VkShaderModule fragmentShaderModule = VK_NULL_HANDLE;

static void handleRegistry(void *data, struct wl_registry *registry,
                           uint32_t name, const char *interface,
                           uint32_t version);

static const struct wl_registry_listener registryListener = {
    .global = handleRegistry};

static void handleShellPing(void *data, struct xdg_wm_base *shell,
                            uint32_t serial) {
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener shellListener = {.ping =
                                                              handleShellPing};

static void handleShellSurfaceConfigure(void *data,
                                        struct xdg_surface *shellSurface,
                                        uint32_t serial) {
    xdg_surface_ack_configure(shellSurface, serial);

    if (resize) {
        readyToResize = 1;
    }
}

static const struct xdg_surface_listener shellSurfaceListener = {
    .configure = handleShellSurfaceConfigure};

static void handleToplevelConfigure(void *data, struct xdg_toplevel *toplevel,
                                    int32_t width, int32_t height,
                                    struct wl_array *states) {
    if (width != 0 && height != 0) {
        resize = 1;
        newWidth = width;
        newHeight = height;
    }
}

static void handleToplevelClose(void *data, struct xdg_toplevel *toplevel) {
    quit = 1;
}

static const struct xdg_toplevel_listener toplevelListener = {
    .configure = handleToplevelConfigure, .close = handleToplevelClose};

static void handleRegistry(void *data, struct wl_registry *registry,
                           uint32_t name, const char *interface,
                           uint32_t version) {
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        CHECK_WL_RESULT(compositor = wl_registry_bind(
                            registry, name, &wl_compositor_interface, 1));
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) ==
               0) {
        decoration_manager = wl_registry_bind(
            registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        CHECK_WL_RESULT(shell = wl_registry_bind(registry, name,
                                                 &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(shell, &shellListener, NULL);
    }
}

static VkBool32
onError(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT *callbackData,
        void *userData) {
    printf("Vulkan ");

    switch (type) {
    case VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT:
        printf("general ");
        break;
    case VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT:
        printf("validation ");
        break;
    case VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT:
        printf("performance ");
        break;
    }

    switch (severity) {
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
        printf("(verbose): ");
        break;
    default:
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
        printf("(info): ");
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
        printf("(warning): ");
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
        printf("(error): ");
        break;
    }

    printf("%s\n", callbackData->pMessage);

    return 0;
}

void alloc_buffer_cb(void *opaque, struct funnel_stream *stream,
                     struct funnel_buffer *buf) {
    /*
    VkImage image;
    VkResult result;
    int ret = funnel_buffer_get_vk_image(buf, &image);
    assert(ret == 0);
    assert(image);

    VkFormat format;
    ret = funnel_buffer_get_vk_format(buf, &format, NULL);
    assert(ret == 0);
    assert(format != VK_FORMAT_UNDEFINED);

    VkImageView view;
    VkImageViewCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .components.r = VK_COMPONENT_SWIZZLE_IDENTITY,
        .components.g = VK_COMPONENT_SWIZZLE_IDENTITY,
        .components.b = VK_COMPONENT_SWIZZLE_IDENTITY,
        .components.a = VK_COMPONENT_SWIZZLE_IDENTITY,
        .subresourceRange.baseMipLevel = 0,
        .subresourceRange.levelCount = 1,
        .subresourceRange.baseArrayLayer = 0,
        .subresourceRange.layerCount = 1,
        .image = image,
        .format = format,
        .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    };

    CHECK_VK_RESULT(vkCreateImageView(device, &createInfo, NULL, &view));

    funnel_buffer_set_user_data(buf, view);
    */
}

void free_buffer_cb(void *opaque, struct funnel_stream *stream,
                    struct funnel_buffer *buf) {
    VkImageView view = funnel_buffer_get_user_data(buf);
    if (view)
        vkDestroyImageView(device, view, NULL);
}

static void createSwapchain(VkPresentModeKHR presentMode) {
    VkResult result;

    {
        VkSurfaceCapabilitiesKHR capabilities;
        CHECK_VK_RESULT(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            physDevice, vulkanSurface, &capabilities));

        uint32_t formatCount;
        CHECK_VK_RESULT(vkGetPhysicalDeviceSurfaceFormatsKHR(
            physDevice, vulkanSurface, &formatCount, NULL));

        VkSurfaceFormatKHR formats[formatCount];
        CHECK_VK_RESULT(vkGetPhysicalDeviceSurfaceFormatsKHR(
            physDevice, vulkanSurface, &formatCount, formats));

        VkSurfaceFormatKHR chosenFormat = formats[0];

        for (uint32_t i = 0; i < formatCount; i++) {
            if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
                chosenFormat = formats[i];
                break;
            }
        }

        format = chosenFormat.format;

        imageCount = capabilities.minImageCount + 1 < capabilities.maxImageCount
                         ? capabilities.minImageCount + 1
                         : capabilities.minImageCount;

        VkSwapchainCreateInfoKHR createInfo = {0};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = vulkanSurface;
        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = chosenFormat.format;
        createInfo.imageColorSpace = chosenFormat.colorSpace;
        createInfo.imageExtent.width = width;
        createInfo.imageExtent.height = height;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.preTransform = capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = 1;

        CHECK_VK_RESULT(
            vkCreateSwapchainKHR(device, &createInfo, NULL, &swapchain));
    }

    {
        VkAttachmentDescription attachment = {0};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        VkAttachmentReference attachmentRef = {0};
        attachmentRef.attachment = 0;
        attachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {0};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &attachmentRef;

        VkRenderPassCreateInfo createInfo = {0};
        createInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        createInfo.flags = 0;
        createInfo.attachmentCount = 1;
        createInfo.pAttachments = &attachment;
        createInfo.subpassCount = 1;
        createInfo.pSubpasses = &subpass;

        CHECK_VK_RESULT(
            vkCreateRenderPass(device, &createInfo, NULL, &renderPass));
    }

    CHECK_VK_RESULT(
        vkGetSwapchainImagesKHR(device, swapchain, &imageCount, NULL));

    VkImage images[imageCount];
    CHECK_VK_RESULT(
        vkGetSwapchainImagesKHR(device, swapchain, &imageCount, images));

    elements = malloc(imageCount * sizeof(struct SwapchainElement));

    for (uint32_t i = 0; i < imageCount; i++) {
        {
            VkCommandBufferAllocateInfo allocInfo = {0};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = commandPool;
            allocInfo.commandBufferCount = 1;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;

            vkAllocateCommandBuffers(device, &allocInfo,
                                     &elements[i].commandBuffer);
        }

        elements[i].image = images[i];

        {
            VkImageViewCreateInfo createInfo = {0};
            createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
            createInfo.subresourceRange.baseMipLevel = 0;
            createInfo.subresourceRange.levelCount = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount = 1;
            createInfo.image = elements[i].image;
            createInfo.format = format;
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

            CHECK_VK_RESULT(vkCreateImageView(device, &createInfo, NULL,
                                              &elements[i].imageView));
        }

        {
            VkFramebufferCreateInfo createInfo = {0};
            createInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            createInfo.renderPass = renderPass;
            createInfo.attachmentCount = 1;
            createInfo.pAttachments = &elements[i].imageView;
            createInfo.width = width;
            createInfo.height = height;
            createInfo.layers = 1;

            CHECK_VK_RESULT(vkCreateFramebuffer(device, &createInfo, NULL,
                                                &elements[i].framebuffer));
        }

        {
            VkSemaphoreCreateInfo createInfo = {0};
            createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

            CHECK_VK_RESULT(vkCreateSemaphore(device, &createInfo, NULL,
                                              &elements[i].startSemaphore));
        }

        {
            VkSemaphoreCreateInfo createInfo = {0};
            createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

            CHECK_VK_RESULT(vkCreateSemaphore(device, &createInfo, NULL,
                                              &elements[i].endSemaphore));
        }

        {
            VkFenceCreateInfo createInfo = {0};
            createInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            createInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

            CHECK_VK_RESULT(
                vkCreateFence(device, &createInfo, NULL, &elements[i].fence));
        }

        elements[i].lastFence = VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertexShaderModule,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragmentShaderModule,
            .pName = "main",
        }};

    VkPipelineVertexInputStateCreateInfo vertexInputInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 0,
        .vertexAttributeDescriptionCount = 0,
    };
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };
    VkViewport viewport = {
        .width = width,
        .height = height,
    };
    VkRect2D scissor = {{0, 0}, {width, height}};
    VkPipelineViewportStateCreateInfo viewportState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,

        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
    };

    VkPipelineColorBlendAttachmentState colorBlendAttachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_FALSE,
    };

    VkPipelineMultisampleStateCreateInfo multisampling = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .minSampleShading = 1.0f,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };

    VkPipelineColorBlendStateCreateInfo colorBlending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &colorBlendAttachment,
    };

    // setup push constants
    VkPushConstantRange push_constant = {
        .offset = 0,
        .size = sizeof(struct PushConstants),
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    };

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .flags = 0,
        .setLayoutCount = 0,
        .pSetLayouts = NULL,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant,
    };

    CHECK_VK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutInfo, NULL,
                                           &pipelineLayout));

    VkGraphicsPipelineCreateInfo pipelineInfo = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,

        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertexInputInfo,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &colorBlending,
        .layout = pipelineLayout,
        .renderPass = renderPass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
    };

    CHECK_VK_RESULT(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                              &pipelineInfo, NULL, &pipeline));
}

static void destroySwapchain() {
    for (uint32_t i = 0; i < imageCount; i++) {
        vkDestroyFence(device, elements[i].fence, NULL);
        vkDestroySemaphore(device, elements[i].endSemaphore, NULL);
        vkDestroySemaphore(device, elements[i].startSemaphore, NULL);
        vkDestroyFramebuffer(device, elements[i].framebuffer, NULL);
        vkDestroyImageView(device, elements[i].imageView, NULL);
        vkFreeCommandBuffers(device, commandPool, 1,
                             &elements[i].commandBuffer);
    }

    free(elements);

    vkDestroyRenderPass(device, renderPass, NULL);

    vkDestroySwapchainKHR(device, swapchain, NULL);

    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipelineLayout, NULL);
}

void loadShader(const uint32_t *buffer, size_t size, VkShaderModule *module) {
    VkResult result;

    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.pNext = NULL;

    // codeSize has to be in bytes, so multiply the ints in the buffer by size
    // of int to know the real size of the buffer
    createInfo.codeSize = size;
    createInfo.pCode = buffer;

    // check that the creation goes well.
    VkShaderModule shaderModule;
    CHECK_VK_RESULT(
        vkCreateShaderModule(device, &createInfo, NULL, &shaderModule));
    *module = shaderModule;
}

int main(int argc, char **argv) {
    enum funnel_mode mode = FUNNEL_ASYNC;
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

    if (argc > 1 && !strcmp(argv[1], "-async")) {
        mode = FUNNEL_ASYNC;
        presentMode = VK_PRESENT_MODE_FIFO_KHR;
    } else if (argc > 1 && !strcmp(argv[1], "-single")) {
        mode = FUNNEL_SINGLE_BUFFERED;
        presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (argc > 1 && !strcmp(argv[1], "-double")) {
        mode = FUNNEL_DOUBLE_BUFFERED;
        presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (argc > 1 && !strcmp(argv[1], "-synchronous")) {
        mode = FUNNEL_SYNCHRONOUS;
        presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    }

    CHECK_WL_RESULT(display = wl_display_connect(NULL));

    CHECK_WL_RESULT(registry = wl_display_get_registry(display));
    wl_registry_add_listener(registry, &registryListener, NULL);
    wl_display_roundtrip(display);

    CHECK_WL_RESULT(surface = wl_compositor_create_surface(compositor));

    CHECK_WL_RESULT(shellSurface = xdg_wm_base_get_xdg_surface(shell, surface));
    xdg_surface_add_listener(shellSurface, &shellSurfaceListener, NULL);

    CHECK_WL_RESULT(toplevel = xdg_surface_get_toplevel(shellSurface));
    xdg_toplevel_add_listener(toplevel, &toplevelListener, NULL);

    CHECK_WL_RESULT(decoration =
                        zxdg_decoration_manager_v1_get_toplevel_decoration(
                            decoration_manager, toplevel));

    xdg_toplevel_set_title(toplevel, appName);
    xdg_toplevel_set_app_id(toplevel, appName);
    zxdg_toplevel_decoration_v1_set_mode(
        decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);

    wl_surface_commit(surface);
    wl_display_roundtrip(display);
    wl_surface_commit(surface);

    VkResult result;

    {
        VkApplicationInfo appInfo = {0};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = appName;
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.pEngineName = appName;
        appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.apiVersion = API_VERSION;

        VkInstanceCreateInfo createInfo = {0};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount =
            sizeof(instanceExtensionNames) / sizeof(const char *);
        createInfo.ppEnabledExtensionNames = instanceExtensionNames;

        size_t foundLayers = 0;

        uint32_t deviceLayerCount;
        CHECK_VK_RESULT(
            vkEnumerateInstanceLayerProperties(&deviceLayerCount, NULL));

        VkLayerProperties *layerProperties =
            malloc(deviceLayerCount * sizeof(VkLayerProperties));
        CHECK_VK_RESULT(vkEnumerateInstanceLayerProperties(&deviceLayerCount,
                                                           layerProperties));

        for (uint32_t i = 0; i < deviceLayerCount; i++) {
            for (size_t j = 0; j < sizeof(layerNames) / sizeof(const char *);
                 j++) {
                if (strcmp(layerProperties[i].layerName, layerNames[j]) == 0) {
                    foundLayers++;
                }
            }
        }

        free(layerProperties);

        if (foundLayers >= sizeof(layerNames) / sizeof(const char *)) {
            createInfo.enabledLayerCount =
                sizeof(layerNames) / sizeof(const char *);
            createInfo.ppEnabledLayerNames = layerNames;
        }

        CHECK_VK_RESULT(vkCreateInstance(&createInfo, NULL, &instance));
    }

    {
        VkDebugUtilsMessengerCreateInfoEXT createInfo = {0};
        createInfo.sType =
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = onError;

        CHECK_VK_RESULT(GET_EXTENSION_FUNCTION(vkCreateDebugUtilsMessengerEXT)(
            instance, &createInfo, NULL, &debugMessenger));
    }

    {
        VkWaylandSurfaceCreateInfoKHR createInfo = {0};
        createInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
        createInfo.display = display;
        createInfo.surface = surface;

        CHECK_VK_RESULT(vkCreateWaylandSurfaceKHR(instance, &createInfo, NULL,
                                                  &vulkanSurface));
    }

    uint32_t physDeviceCount;
    vkEnumeratePhysicalDevices(instance, &physDeviceCount, NULL);

    VkPhysicalDevice physDevices[physDeviceCount];
    vkEnumeratePhysicalDevices(instance, &physDeviceCount, physDevices);

    uint32_t bestScore = 0;

    for (uint32_t i = 0; i < physDeviceCount; i++) {
        VkPhysicalDevice device = physDevices[i];

        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(device, &properties);

        uint32_t score;

        switch (properties.deviceType) {
        default:
            continue;
        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
            score = 1;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            score = 4;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            score = 5;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            score = 3;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            score = 2;
            break;
        }

        if (score > bestScore) {
            physDevice = device;
            bestScore = score;
        }
    }

    {
        uint32_t queueFamilyCount;
        vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount,
                                                 NULL);

        VkQueueFamilyProperties queueFamilies[queueFamilyCount];
        vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount,
                                                 queueFamilies);

        for (uint32_t i = 0; i < queueFamilyCount; i++) {
            VkBool32 present = 0;

            CHECK_VK_RESULT(vkGetPhysicalDeviceSurfaceSupportKHR(
                physDevice, i, vulkanSurface, &present));

            if (present &&
                (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                queueFamilyIndex = i;
                break;
            }
        }

        float priority = 1;

        VkDeviceQueueCreateInfo queueCreateInfo = {0};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &priority;

        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT swapchain_maint = {
            .sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
            .swapchainMaintenance1 = VK_TRUE,
        };

        VkDeviceCreateInfo createInfo = {0};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = &swapchain_maint;
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &queueCreateInfo;
        createInfo.enabledExtensionCount =
            sizeof(deviceExtensionNames) / sizeof(const char *);
        createInfo.ppEnabledExtensionNames = deviceExtensionNames;

        uint32_t deviceLayerCount;
        CHECK_VK_RESULT(vkEnumerateDeviceLayerProperties(
            physDevice, &deviceLayerCount, NULL));

        VkLayerProperties *layerProperties =
            malloc(deviceLayerCount * sizeof(VkLayerProperties));
        CHECK_VK_RESULT(vkEnumerateDeviceLayerProperties(
            physDevice, &deviceLayerCount, layerProperties));

        size_t foundLayers = 0;

        for (uint32_t i = 0; i < deviceLayerCount; i++) {
            for (size_t j = 0; j < sizeof(layerNames) / sizeof(const char *);
                 j++) {
                if (strcmp(layerProperties[i].layerName, layerNames[j]) == 0) {
                    foundLayers++;
                }
            }
        }

        free(layerProperties);

        if (foundLayers >= sizeof(layerNames) / sizeof(const char *)) {
            createInfo.enabledLayerCount =
                sizeof(layerNames) / sizeof(const char *);
            createInfo.ppEnabledLayerNames = layerNames;
        }

        CHECK_VK_RESULT(vkCreateDevice(physDevice, &createInfo, NULL, &device));

        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);
    }

    {
        VkCommandPoolCreateInfo createInfo = {0};
        createInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        createInfo.queueFamilyIndex = queueFamilyIndex;
        createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        CHECK_VK_RESULT(
            vkCreateCommandPool(device, &createInfo, NULL, &commandPool));
    }

    loadShader(triangle_vert, sizeof(triangle_vert), &vertexShaderModule);
    loadShader(triangle_frag, sizeof(triangle_frag), &fragmentShaderModule);
    createSwapchain(presentMode);

    int frame = 0;

    int ret;
    struct funnel_ctx *ctx;
    struct funnel_stream *stream;

    ret = funnel_init(&ctx);
    assert(ret == 0);

    ret = funnel_stream_create(ctx, "Funnel Test", &stream);
    assert(ret == 0);

    funnel_stream_set_buffer_callbacks(stream, alloc_buffer_cb, free_buffer_cb,
                                       NULL);

    ret = funnel_stream_init_vulkan(stream, instance, physDevice, device);
    assert(ret == 0);

    ret = funnel_stream_set_size(stream, width, height);
    assert(ret == 0);

    ret = funnel_stream_set_mode(stream, mode);
    assert(ret == 0);

    ret =
        funnel_stream_set_rate(stream, FUNNEL_RATE_VARIABLE,
                               FUNNEL_FRACTION(1, 1), FUNNEL_FRACTION(1000, 1));
    assert(ret == 0);

    ret = funnel_stream_vk_set_usage(stream, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    assert(ret == 0);

    bool have_format = false;
    ret = funnel_stream_vk_add_format(stream, VK_FORMAT_R8G8B8A8_SRGB, true,
                                      VK_FORMAT_FEATURE_BLIT_DST_BIT);
    have_format |= ret == 0;
    ret = funnel_stream_vk_add_format(stream, VK_FORMAT_B8G8R8A8_SRGB, true,
                                      VK_FORMAT_FEATURE_BLIT_DST_BIT);
    have_format |= ret == 0;
    ret = funnel_stream_vk_add_format(stream, VK_FORMAT_R8G8B8A8_SRGB, false,
                                      VK_FORMAT_FEATURE_BLIT_DST_BIT);
    have_format |= ret == 0;
    ret = funnel_stream_vk_add_format(stream, VK_FORMAT_B8G8R8A8_SRGB, false,
                                      VK_FORMAT_FEATURE_BLIT_DST_BIT);
    have_format |= ret == 0;

    assert(have_format);

    ret = funnel_stream_configure(stream);
    assert(ret == 0);

    ret = funnel_stream_start(stream);
    assert(ret == 0);

    while (!quit) {
        if (readyToResize && resize) {
            bool changed = width != newWidth || height != newHeight;
            width = newWidth;
            height = newHeight;

            CHECK_VK_RESULT(vkDeviceWaitIdle(device));

            destroySwapchain();
            createSwapchain(presentMode);

            currentFrame = 0;
            imageIndex = 0;

            readyToResize = 0;
            resize = 0;

            wl_surface_commit(surface);

            if (changed) {
                ret = funnel_stream_set_size(stream, width, height);
                assert(ret == 0);
                ret = funnel_stream_configure(stream);
                assert(ret == 0);
            }
        }

        struct funnel_buffer *buf;
        ret = funnel_stream_dequeue(stream, &buf);

        struct SwapchainElement *currentElement = &elements[currentFrame];

        CHECK_VK_RESULT(
            vkWaitForFences(device, 1, &currentElement->fence, 1, UINT64_MAX));
        result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                       currentElement->startSemaphore, NULL,
                                       &imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            CHECK_VK_RESULT(vkDeviceWaitIdle(device));
            destroySwapchain();
            createSwapchain(presentMode);
            continue;
        } else if (result < 0) {
            CHECK_VK_RESULT(result);
        }

        struct SwapchainElement *element = &elements[imageIndex];

        if (element->lastFence) {
            CHECK_VK_RESULT(
                vkWaitForFences(device, 1, &element->lastFence, 1, UINT64_MAX));
        }

        element->lastFence = currentElement->fence;

        CHECK_VK_RESULT(vkResetFences(device, 1, &currentElement->fence));

        VkCommandBufferBeginInfo beginInfo = {0};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        CHECK_VK_RESULT(
            vkBeginCommandBuffer(element->commandBuffer, &beginInfo));

        {
            VkClearValue clearValue = {{{0.0f, 0.0f, 0.0f, 0.0f}}};

            VkRenderPassBeginInfo beginInfo = {0};
            beginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            beginInfo.renderPass = renderPass;
            beginInfo.framebuffer = element->framebuffer;
            beginInfo.renderArea.offset.x = 0;
            beginInfo.renderArea.offset.y = 0;
            beginInfo.renderArea.extent.width = width;
            beginInfo.renderArea.extent.height = height;
            beginInfo.clearValueCount = 1;
            beginInfo.pClearValues = &clearValue;

            vkCmdBeginRenderPass(element->commandBuffer, &beginInfo,
                                 VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(element->commandBuffer,
                              VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

            struct PushConstants constants = {frame++};
            vkCmdPushConstants(element->commandBuffer, pipelineLayout,
                               VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(constants),
                               &constants);
            vkCmdDraw(element->commandBuffer, 3, 1, 0, 0);
            vkCmdEndRenderPass(element->commandBuffer);
            if (buf) {
                uint32_t bwidth, bheight;

                funnel_buffer_get_size(buf, &bwidth, &bheight);

                VkImage image;
                int ret = funnel_buffer_get_vk_image(buf, &image);
                assert(ret == 0);
                assert(image);
                VkImageBlit region = {
                    .srcSubresource =
                        {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .mipLevel = 0,
                            .baseArrayLayer = 0,
                            .layerCount = 1,
                        },
                    .srcOffsets = {{0, 0, 0}, {width, height, 1}},
                    .dstSubresource =
                        {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .mipLevel = 0,
                            .baseArrayLayer = 0,
                            .layerCount = 1,
                        },
                    .dstOffsets = {{0, 0, 0}, {bwidth, bheight, 1}},
                };

                vkCmdBlitImage(element->commandBuffer, element->image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                               VK_FILTER_NEAREST);
            }

            VkImageMemoryBarrier barrier = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dstAccessMask = 0,
                .srcQueueFamilyIndex = queueFamilyIndex,
                .dstQueueFamilyIndex = queueFamilyIndex,
                .image = element->image,
                .subresourceRange =
                    {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel = 0,
                        .levelCount = 1,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
            };

            vkCmdPipelineBarrier(element->commandBuffer,
                                 VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                                 VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, NULL,
                                 0, NULL, 1, &barrier);
        }

        CHECK_VK_RESULT(vkEndCommandBuffer(element->commandBuffer));

        const VkPipelineStageFlags waitStage[2] = {
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};

        VkSemaphore wait_semaphores[2] = {currentElement->startSemaphore};
        VkSemaphore signal_semaphores[2] = {currentElement->endSemaphore};

        VkFence submit_fence = VK_NULL_HANDLE;

        if (buf) {
            ret = funnel_buffer_get_vk_semaphores(buf, &wait_semaphores[1],
                                                  &signal_semaphores[1]);
            assert(ret == 0);
            ret = funnel_buffer_get_vk_fence(buf, &submit_fence);
            assert(ret == 0);
        }

        VkSubmitInfo submitInfo = {0};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = buf ? 2 : 1;
        submitInfo.pWaitSemaphores = wait_semaphores;
        submitInfo.pWaitDstStageMask = waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &element->commandBuffer;
        submitInfo.signalSemaphoreCount = buf ? 2 : 1;
        submitInfo.pSignalSemaphores = signal_semaphores;

        CHECK_VK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, submit_fence));

        VkPresentInfoKHR presentInfo = {0};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &currentElement->endSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &imageIndex;

        VkSwapchainPresentFenceInfoEXT swapchainInfo = {
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT,
            .swapchainCount = 1,
            .pFences = &currentElement->fence,
        };

        presentInfo.pNext = &swapchainInfo;

        result = vkQueuePresentKHR(queue, &presentInfo);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
            CHECK_VK_RESULT(vkDeviceWaitIdle(device));
            destroySwapchain();
            createSwapchain(presentMode);
        } else if (result < 0) {
            CHECK_VK_RESULT(result);
        }

        currentFrame = (currentFrame + 1) % imageCount;

        if (buf) {
            ret = funnel_stream_enqueue(stream, buf);
            if (ret < 0)
                fprintf(stderr, "Queue failed: %d\n", ret);
            else if (ret != 1)
                fprintf(stderr,
                        "Buffer dropped (stream renegotiated or paused)\n");

            assert(ret >= 0);
        }

        wl_display_roundtrip(display);
    }

    CHECK_VK_RESULT(vkDeviceWaitIdle(device));

    ret = funnel_stream_stop(stream);
    assert(ret == 0);

    funnel_stream_destroy(stream);

    funnel_shutdown(ctx);

    destroySwapchain();

    vkDestroyShaderModule(device, vertexShaderModule, NULL);
    vkDestroyShaderModule(device, fragmentShaderModule, NULL);

    vkDestroyCommandPool(device, commandPool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroySurfaceKHR(instance, vulkanSurface, NULL);
    GET_EXTENSION_FUNCTION(vkDestroyDebugUtilsMessengerEXT)(
        instance, debugMessenger, NULL);
    vkDestroyInstance(instance, NULL);

    xdg_toplevel_destroy(toplevel);
    xdg_surface_destroy(shellSurface);
    wl_surface_destroy(surface);
    xdg_wm_base_destroy(shell);
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);

    return 0;
}
