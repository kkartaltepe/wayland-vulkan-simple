// clang-format off
// # vim: tabstop=2 shiftwidth=2 expandtab
// Build this with:
// $ gcc -g -o demo main.c xdg-shell-protocol.c -lwayland-client -lpthread -lvulkan -lm
// Generate the xdg-shell files from protocols with
// $ wayland-scanner private-code < /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml > xdg-shell-protocol.c
// $ wayland-scanner client-header < /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml > xdg-shell-client-protocol.h
// Generate the shader binaries with
// $ glslc -o - shader.frag | xxd -i -n frag_spv > shaders.h
// $ glslc -o - shader.vert | xxd -i -n vert_spv >> shaders.h
// clang-format on

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "shaders.h"                   // Only suffering exists in this world.
#include "xdg-shell-client-protocol.h" // True suffering is generated code.

#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>

#define ARRAY_SIZEOF(A) (sizeof(A) / sizeof(A[0]))
#define VK_VALIDATION
#define CLAMP(V, L, H) (V < L ? L : (V > H ? H : V))

// Window system information
struct wsi {
  struct wl_display *display;
  struct wl_compositor *compositor;
  struct wl_surface *surface;
  struct xdg_wm_base *wm;
  struct xdg_surface *xdg_surface;
  struct xdg_toplevel *xdg_toplevel;
  struct {
    VkSurfaceKHR surface;
    VkSurfaceCapabilitiesKHR surfCaps;
    VkSwapchainKHR swapchain;
    VkFormat swapFormat;
    VkColorSpaceKHR swapSpace;
    uint32_t imgCount;
    VkImage swapImg[8];
    VkImageView swapImgView[8];
    VkFramebuffer fb[8];
    bool recreate;
  } vk;

  int32_t w, h;
  bool window_closed;
  bool frame_done;
};

struct vk {
  VkInstance instance;
  VkPhysicalDevice pdev;
  VkPhysicalDeviceMemoryProperties pmem;
  int32_t gfxIdx;
  VkDevice dev;
  VkQueue gfx;
  VkCommandPool cmdPool;
};

struct wsi WSI = {0};
struct vk VK = {0};
VkExtensionProperties vkExtensions[64] = {0};

struct vk_buffer {
  VkBufferCreateInfo ci;
  VkBuffer buf;
  VkDeviceSize alloc_size;
  int32_t memTypeIdx;
  VkDeviceMemory mem;
};

VkExtent2D swapSize() {
  return (VkExtent2D){CLAMP(WSI.w, WSI.vk.surfCaps.minImageExtent.width,
                            WSI.vk.surfCaps.maxImageExtent.width),
                      CLAMP(WSI.h, WSI.vk.surfCaps.minImageExtent.height,
                            WSI.vk.surfCaps.maxImageExtent.height)};
}

int32_t findMemoryIdx(VkPhysicalDeviceMemoryProperties memories,
                      uint32_t allowed_memories,
                      VkMemoryPropertyFlags properties) {
  for (int32_t i = 0; i < memories.memoryTypeCount; i++) {
    if ((allowed_memories & (1 << i)) &&
        (memories.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }
  return -1;
}

struct vk_buffer vk_buffer_new(VkDeviceSize size, VkBufferUsageFlags usage,
                               VkMemoryPropertyFlags properties) {

  struct vk_buffer b = {0};
  b.ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  b.ci.size = size;
  b.ci.usage = usage;
  b.ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkResult result = vkCreateBuffer(VK.dev, &b.ci, NULL, &b.buf);
  assert(result == VK_SUCCESS);

  VkMemoryRequirements reqs;
  vkGetBufferMemoryRequirements(VK.dev, b.buf, &reqs);

  b.alloc_size = reqs.size;
  b.memTypeIdx = findMemoryIdx(VK.pmem, reqs.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  assert(b.memTypeIdx > -1);

  VkMemoryAllocateInfo allocInfo = {0};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = b.alloc_size;
  allocInfo.memoryTypeIndex = b.memTypeIdx;

  result = vkAllocateMemory(VK.dev, &allocInfo, NULL, &b.mem);
  assert(result == VK_SUCCESS);

  vkBindBufferMemory(VK.dev, b.buf, b.mem, 0);

  return b;
}

// xdg_wm_base generic callbacks

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base,
                             uint32_t serial) {
  xdg_wm_base_pong(xdg_wm_base, serial);
}

const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                  uint32_t serial) {
  xdg_surface_ack_configure(xdg_surface, serial);
}

const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

// xdg_wm_base top level window callbacks.

static void xdg_toplevel_configure(void *data,
                                   struct xdg_toplevel *xdg_toplevel, int32_t w,
                                   int32_t h, struct wl_array *states) {

  // our chosen w/h are already fine.
  if (w == 0 && h == 0)
    return;

  printf("Toplevel configured\n");

  // window resized
  if (WSI.w != w || WSI.h != h) {
    WSI.w = w;
    WSI.h = h;

    WSI.vk.recreate = true;
    wl_surface_commit(WSI.surface);
  }
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
  WSI.window_closed = true;
}

struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure,
    .close = xdg_toplevel_close,
};

// wl_registry handling

static void global_registry_handler(void *data, struct wl_registry *registry,
                                    uint32_t id, const char *interface,
                                    uint32_t version) {
  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    WSI.compositor =
        wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    WSI.surface = wl_compositor_create_surface(WSI.compositor);
    // create the unused cursor surface.
    wl_compositor_create_surface(WSI.compositor);
  }
  if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    WSI.wm = wl_registry_bind(registry, id, &xdg_wm_base_interface, 2);
    xdg_wm_base_add_listener(WSI.wm, &xdg_wm_base_listener, NULL);
  }
}

static void global_registry_remover(void *data, struct wl_registry *registry,
                                    uint32_t id) {
  // This section deliberately left blank.
}

const struct wl_registry_listener registry_listener = {
    .global = global_registry_handler,
    .global_remove = global_registry_remover,
};

// forward declare for for use in the callback.
static const struct wl_callback_listener wl_surface_frame_callback_listener;

static void wl_surface_frame_done(void *data, struct wl_callback *cb,
                                  uint32_t time) {
  // Mark callback handled.
  wl_callback_destroy(cb);

  // Register next frame's callback.
  cb = wl_surface_frame(WSI.surface);
  wl_callback_add_listener(cb, &wl_surface_frame_callback_listener, NULL);
  WSI.frame_done = true;
}

static const struct wl_callback_listener wl_surface_frame_callback_listener = {
    .done = wl_surface_frame_done,
};

void *render_thread(void *data) { return 0; }

VkResult recreate_swapchain() {
  for (uint32_t i = 0; i < WSI.vk.imgCount; i++) {
    vkDestroyFramebuffer(VK.dev, WSI.vk.fb[i], NULL);
    WSI.vk.fb[i] = NULL;
    vkDestroyImageView(VK.dev, WSI.vk.swapImgView[i], NULL);
    WSI.vk.swapImgView[i] = NULL;
  }
  vkDestroySwapchainKHR(VK.dev, WSI.vk.swapchain, NULL);
  WSI.vk.swapchain = NULL;

  // TODO: validate our format/colorspace/presentmode
  WSI.vk.swapFormat = VK_FORMAT_B8G8R8A8_SRGB;
  WSI.vk.swapSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

  VkSwapchainCreateInfoKHR createSwapInfo = {0};
  createSwapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  createSwapInfo.surface = WSI.vk.surface;
  createSwapInfo.minImageCount = 4;
  createSwapInfo.imageFormat = WSI.vk.swapFormat;
  createSwapInfo.imageColorSpace = WSI.vk.swapSpace;
  createSwapInfo.imageExtent = swapSize();
  createSwapInfo.imageArrayLayers = 1;
  createSwapInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  createSwapInfo.preTransform = WSI.vk.surfCaps.currentTransform;
  createSwapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  createSwapInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  createSwapInfo.clipped = VK_TRUE;
  // Only same queue gfx/present.
  createSwapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkResult result =
      vkCreateSwapchainKHR(VK.dev, &createSwapInfo, NULL, &WSI.vk.swapchain);
  assert(result == VK_SUCCESS);

  WSI.vk.imgCount = 8;
  vkGetSwapchainImagesKHR(VK.dev, WSI.vk.swapchain, &WSI.vk.imgCount,
                          &WSI.vk.swapImg[0]);

  for (uint32_t i = 0; i < WSI.vk.imgCount; i++) {
    VkImageViewCreateInfo createViewInfo = {0};
    createViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    createViewInfo.image = WSI.vk.swapImg[i];
    createViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    createViewInfo.format = WSI.vk.swapFormat;
    createViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    createViewInfo.subresourceRange.baseMipLevel = 0;
    createViewInfo.subresourceRange.levelCount = 1;
    createViewInfo.subresourceRange.baseArrayLayer = 0;
    createViewInfo.subresourceRange.layerCount = 1;
    result = vkCreateImageView(VK.dev, &createViewInfo, NULL,
                               &WSI.vk.swapImgView[i]);
    assert(result == VK_SUCCESS);
  }

  return result;
}

int main(int argc, char *argv[]) {
  WSI.display = wl_display_connect(NULL);
  assert(WSI.display);
  struct wl_registry *registry = wl_display_get_registry(WSI.display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(WSI.display);

  // Create our surface and add xdg_shell roles so it will be displayed.
  WSI.w = 300;
  WSI.h = 300;
  WSI.xdg_surface = xdg_wm_base_get_xdg_surface(WSI.wm, WSI.surface);
  xdg_surface_add_listener(WSI.xdg_surface, &xdg_surface_listener, NULL);
  WSI.xdg_toplevel = xdg_surface_get_toplevel(WSI.xdg_surface);
  xdg_toplevel_add_listener(WSI.xdg_toplevel, &xdg_toplevel_listener, NULL);
  xdg_toplevel_set_title(WSI.xdg_toplevel, "Wayland VK window");

  struct wl_callback *cb = wl_surface_frame(WSI.surface);
  wl_callback_add_listener(cb, &wl_surface_frame_callback_listener, NULL);

  // Test vulkan works
  uint32_t extensionCount = 64;
  vkEnumerateInstanceExtensionProperties(NULL, &extensionCount, vkExtensions);

  VkApplicationInfo appInfo = {0};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName = "Vulkan Wayland Demo";
  appInfo.applicationVersion = VK_MAKE_VERSION(0, 0, 1);
  appInfo.apiVersion =
      VK_API_VERSION_1_0; // Vulkan 1.0 drivers will refuse other versions.

  // MoltenVK requires VK_KHR_portability_enumeration for nonconformance.
  const char *waylandExts[2] = {"VK_KHR_wayland_surface", "VK_KHR_surface"};
  const char *validationLayers[1] = {"VK_LAYER_KHRONOS_validation"};
  VkInstanceCreateInfo createInstInfo = {0};
  createInstInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInstInfo.pApplicationInfo = &appInfo;
  createInstInfo.enabledExtensionCount = ARRAY_SIZEOF(waylandExts);
  createInstInfo.ppEnabledExtensionNames = waylandExts;
#ifdef VK_VALIDATION
  createInstInfo.enabledLayerCount = ARRAY_SIZEOF(validationLayers);
  createInstInfo.ppEnabledLayerNames = validationLayers;
#endif

  VkResult result = vkCreateInstance(&createInstInfo, NULL, &VK.instance);
  assert(!result);

  uint32_t deviceCount = 8;
  VkPhysicalDevice pDevices[8];
  vkEnumeratePhysicalDevices(VK.instance, &deviceCount, pDevices);
  assert(deviceCount >= 1);
  VK.pdev = pDevices[0];

  // If physical device info is needed.
  VkPhysicalDeviceProperties deviceProperties;
  vkGetPhysicalDeviceProperties(VK.pdev, &deviceProperties);
  VkPhysicalDeviceFeatures deviceFeatures;
  vkGetPhysicalDeviceFeatures(VK.pdev, &deviceFeatures);
  vkGetPhysicalDeviceMemoryProperties(VK.pdev, &VK.pmem);

  // Setup the WSI surface so we can check it against queues.
  VkWaylandSurfaceCreateInfoKHR surfCreateInfo = {0};
  surfCreateInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
  surfCreateInfo.display = WSI.display;
  surfCreateInfo.surface = WSI.surface;
  result = vkCreateWaylandSurfaceKHR(VK.instance, &surfCreateInfo, NULL,
                                     &WSI.vk.surface);
  assert(result == VK_SUCCESS);

  // Find graphics queue
  uint32_t queueFamilyCount = 8;
  VkQueueFamilyProperties queueFamilies[8] = {0};
  vkGetPhysicalDeviceQueueFamilyProperties(VK.pdev, &queueFamilyCount,
                                           queueFamilies);
  VK.gfxIdx = -1;
  for (uint32_t i = 0; i < queueFamilyCount; i++) {
    VkBool32 presentSupport = false;
    vkGetPhysicalDeviceSurfaceSupportKHR(VK.pdev, i, WSI.vk.surface,
                                         &presentSupport);
    if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT && presentSupport &&
        VK.gfxIdx == -1) {
      VK.gfxIdx = i;
    }
  }
  assert(VK.gfxIdx != -1);

  VkDeviceQueueCreateInfo queueCreateInfo = {0};
  queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queueCreateInfo.queueFamilyIndex = VK.gfxIdx;
  queueCreateInfo.queueCount = 1;
  float prio = 1.0f;
  queueCreateInfo.pQueuePriorities = &prio;

  VkPhysicalDeviceFeatures enabledDeviceFeatures = {0};

  const char *deviceExts[1] = {"VK_KHR_swapchain"};
  VkDeviceCreateInfo createDevInfo = {0};
  createDevInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  createDevInfo.pQueueCreateInfos = &queueCreateInfo;
  createDevInfo.queueCreateInfoCount = 1;
  createDevInfo.pEnabledFeatures = &enabledDeviceFeatures;
#ifdef VK_VALIDATION
  // Required before vulkan 1.1
  // Along with device specific extensions.
  createDevInfo.enabledLayerCount = ARRAY_SIZEOF(validationLayers);
  createDevInfo.ppEnabledLayerNames = validationLayers;
#endif
  createDevInfo.enabledExtensionCount = ARRAY_SIZEOF(deviceExts);
  createDevInfo.ppEnabledExtensionNames = deviceExts;

  result = vkCreateDevice(VK.pdev, &createDevInfo, NULL, &VK.dev);
  assert(result == VK_SUCCESS);
  vkGetDeviceQueue(VK.dev, VK.gfxIdx, 0, &VK.gfx);
  assert(VK.gfx != NULL);

  uint32_t swapFormatsCount = 128;
  VkSurfaceFormatKHR swapFormats[128] = {0};
  vkGetPhysicalDeviceSurfaceFormatsKHR(VK.pdev, WSI.vk.surface,
                                       &swapFormatsCount, swapFormats);
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VK.pdev, WSI.vk.surface,
                                            &WSI.vk.surfCaps);
  uint32_t presentModeCount = 8;
  VkPresentModeKHR presentModes[8] = {0};
  vkGetPhysicalDeviceSurfacePresentModesKHR(VK.pdev, WSI.vk.surface,
                                            &presentModeCount, presentModes);
  assert(presentModeCount > 0);
  assert(swapFormatsCount > 0);

  // Get our top level configured for our swapchain.
  wl_surface_set_buffer_scale(WSI.surface, 1);
  wl_surface_commit(WSI.surface);
  wl_display_dispatch(WSI.display);
  wl_display_roundtrip(WSI.display);

  // TODO: validate our format/colorspace/presentmode
  recreate_swapchain();

  // Now we can build some shaders and pipelines.

  // Create some descriptor sets
  // For an OpenGL Experience (tm): You want 32 textures, 16 images, 24 UBOs,
  // etc. and map bindings into these slots.
  VkDescriptorSetLayoutBinding uboLayoutBinding = {0};
  uboLayoutBinding.binding = 0;
  uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  uboLayoutBinding.descriptorCount = 1;
  uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

  VkDescriptorSetLayoutCreateInfo layoutInfo = {0};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 1;
  layoutInfo.pBindings = &uboLayoutBinding;

  VkDescriptorSetLayout descriptorSetLayout;
  result = vkCreateDescriptorSetLayout(VK.dev, &layoutInfo, NULL,
                                       &descriptorSetLayout);
  assert(result == VK_SUCCESS);

  // Pools for all the descriptors we can bind into our layout(s).
  // Assuming only 1 frame in flight, more needed for more frames.
  VkDescriptorPoolSize poolSize = {0};
  poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  poolSize.descriptorCount = 1;

  VkDescriptorPoolCreateInfo descPoolInfo = {0};
  descPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  descPoolInfo.poolSizeCount = 1;
  descPoolInfo.pPoolSizes = &poolSize;
  // how many DescriptorSets we can allocate out of this pool.
  descPoolInfo.maxSets = 1;

  VkDescriptorPool descriptorPool;
  result = vkCreateDescriptorPool(VK.dev, &descPoolInfo, NULL, &descriptorPool);
  assert(result == VK_SUCCESS);

  // Finally allocate the set from the pool for our layout.
  VkDescriptorSetAllocateInfo descSetAllocInfo = {0};
  descSetAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  descSetAllocInfo.descriptorPool = descriptorPool;
  descSetAllocInfo.descriptorSetCount = 1;
  descSetAllocInfo.pSetLayouts = &descriptorSetLayout;

  VkDescriptorSet descSet;
  result = vkAllocateDescriptorSets(VK.dev, &descSetAllocInfo, &descSet);
  assert(result == VK_SUCCESS);

  // Alright actual shader stuff now.
  VkShaderModule fragShader, vertShader;
  VkShaderModuleCreateInfo createShaderInfo = {0};
  createShaderInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;

  createShaderInfo.codeSize = frag_spv_len;
  createShaderInfo.pCode = (const uint32_t *)frag_spv; // Legal?
  assert(vkCreateShaderModule(VK.dev, &createShaderInfo, NULL, &fragShader) ==
         VK_SUCCESS);

  createShaderInfo.codeSize = vert_spv_len;
  createShaderInfo.pCode = (const uint32_t *)vert_spv;
  assert(vkCreateShaderModule(VK.dev, &createShaderInfo, NULL, &vertShader) ==
         VK_SUCCESS);

  VkPipelineShaderStageCreateInfo shaderStages[2] = {0};
  shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  shaderStages[0].module = vertShader;
  shaderStages[0].pName = "main";
  shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shaderStages[1].module = fragShader;
  shaderStages[1].pName = "main";

  // Avoid setting VkPipelineViewportStateCreateInfo
  VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                     VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamicState = {0};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = ARRAY_SIZEOF(dynamicStates);
  dynamicState.pDynamicStates = dynamicStates;

  // Representation of the packed vertex stage input for use in configuring
  // shader input. Also VUID-VkVertexInputBindingDescription-stride-04456
  struct VData {
    struct {
      float p1;
      float p2;
    } pos;
    struct {
      float c1;
      float c2;
      float c3;
    } col;
  };
  struct VData vertexIn[3] = {
      {{0.0f, -0.5f}, {1.0f, 0.0f, 0.0f}},
      {{0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}},
      {{-0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}},
  };
  struct MData {
    float m[16];
  };
  struct MData matrixIn = {
      // clang-format off
      1.f, 0.f, 0.f, 0.f,
      0.f, 1.f, 0.f, 0.f,
      0.f, 0.f, 1.f, 0.f,
      0.f, 0.f, 0.f, 1.f,
      // clang-format on
  };

  VkVertexInputBindingDescription vibd = {0};
  vibd.binding = 0;
  // TODO: Look up how engines actually figure this for aggregate
  // types.
  vibd.stride = sizeof(struct VData);
  vibd.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

  VkVertexInputAttributeDescription viad[2] = {0};
  viad[0].binding = 0;
  viad[0].location = 0;
  viad[0].format = VK_FORMAT_R32G32_SFLOAT;
  viad[0].offset = offsetof(struct VData, pos);

  viad[1].binding = 0;
  viad[1].location = 1;
  viad[1].format = VK_FORMAT_R32G32B32_SFLOAT;
  viad[1].offset = offsetof(struct VData, col);

  VkPipelineVertexInputStateCreateInfo vertexInputInfo = {0};
  vertexInputInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInputInfo.vertexBindingDescriptionCount = 1;
  vertexInputInfo.pVertexBindingDescriptions = &vibd;
  vertexInputInfo.vertexAttributeDescriptionCount = 2;
  vertexInputInfo.pVertexAttributeDescriptions = viad;

  VkPipelineInputAssemblyStateCreateInfo inputAssembly = {0};
  inputAssembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  inputAssembly.primitiveRestartEnable = VK_FALSE;

  VkPipelineViewportStateCreateInfo viewportState = {0};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rasterizer = {0};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
  rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rasterizer.lineWidth = 1.0f;

  // No-op multisample is required.
  VkPipelineMultisampleStateCreateInfo multisampling = {0};
  multisampling.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  /*
  VkPipelineDepthStencilStateCreateInfo;
  */

  VkPipelineColorBlendAttachmentState colorBlendAttachment = {0};
  colorBlendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  colorBlendAttachment.blendEnable = VK_FALSE;

  VkPipelineColorBlendStateCreateInfo colorBlending = {0};
  colorBlending.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlending.logicOpEnable = VK_FALSE;
  colorBlending.logicOp = VK_LOGIC_OP_COPY; // Optional
  colorBlending.attachmentCount = 1;
  colorBlending.pAttachments = &colorBlendAttachment;

  // Empty as vertex data is encoded in the shader.
  VkPipelineLayoutCreateInfo pipelineLayoutInfo = {0};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.setLayoutCount = 1;
  pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
  pipelineLayoutInfo.pushConstantRangeCount = 0; // Optional
  pipelineLayoutInfo.pPushConstantRanges = NULL; // Optional

  VkPipelineLayout pipelineLayout;
  result = vkCreatePipelineLayout(VK.dev, &pipelineLayoutInfo, NULL,
                                  &pipelineLayout);
  assert(result == VK_SUCCESS);

  // Render Pass info
  VkAttachmentDescription colorAttachment = {0};
  // Needs a recreate if swapchain format changes...
  colorAttachment.format = WSI.vk.swapFormat;
  colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
  colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  VkAttachmentReference colorAttachmentRef = {0};
  colorAttachmentRef.attachment = 0; // location = 0 output.
  colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass = {0};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorAttachmentRef;

  VkSubpassDependency dependency = {0};
  // subpass 0 color attachment has a write dependency against ...
  dependency.dstSubpass = 0;
  dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  // swapchain's external access of color attachments.
  dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
  dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependency.srcAccessMask = 0;

  VkRenderPassCreateInfo renderPassInfo = {0};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = 1;
  renderPassInfo.pAttachments = &colorAttachment;
  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpass;
  renderPassInfo.dependencyCount = 1;
  renderPassInfo.pDependencies = &dependency;

  VkRenderPass renderPass;
  vkCreateRenderPass(VK.dev, &renderPassInfo, NULL, &renderPass);
  assert(result == VK_SUCCESS);

  // Finally assemble the pipeline
  VkGraphicsPipelineCreateInfo pipelineInfo = {0};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = shaderStages;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pVertexInputState = &vertexInputInfo;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  // pipelineInfo.pDepthStencilState = NULL;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.pDynamicState = &dynamicState;
  pipelineInfo.layout = pipelineLayout;
  pipelineInfo.renderPass = renderPass;
  pipelineInfo.subpass = 0;

  VkPipeline graphicsPipeline;
  result = vkCreateGraphicsPipelines(VK.dev, VK_NULL_HANDLE, 1, &pipelineInfo,
                                     NULL, &graphicsPipeline);
  assert(result == VK_SUCCESS);

  // Allocate some pipeline input
  struct vk_buffer vertexBuffer =
      vk_buffer_new(sizeof(struct VData) * 3, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  void *data;
  vkMapMemory(VK.dev, vertexBuffer.mem, 0, vertexBuffer.ci.size, 0, &data);
  memcpy(data, vertexIn, (size_t)vertexBuffer.ci.size);
  vkUnmapMemory(VK.dev, vertexBuffer.mem);

  struct vk_buffer matrixBuffer =
      vk_buffer_new(sizeof(struct MData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  vkMapMemory(VK.dev, matrixBuffer.mem, 0, matrixBuffer.ci.size, 0, &data);
  memcpy(data, &matrixIn, (size_t)matrixBuffer.ci.size);
  // Persistent mapping aka "While a range of device memory is host mapped, the
  // application is responsible for synchronizing both device and host access to
  // that memory range."
  //
  // vkUnmapMemory(VK.dev, matrixBuffer.mem);

  // Write out buffers into the shader descriptors
  VkDescriptorBufferInfo bufferInfo = {0};
  bufferInfo.buffer = matrixBuffer.buf;
  bufferInfo.offset = 0;
  bufferInfo.range = matrixBuffer.ci.size;

  VkWriteDescriptorSet descriptorWrite = {0};
  descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptorWrite.dstSet = descSet;
  descriptorWrite.dstBinding = 0; // Remember the binding from the shader?
  descriptorWrite.dstArrayElement = 0;
  descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptorWrite.descriptorCount = 1;
  descriptorWrite.pBufferInfo = &bufferInfo;
  descriptorWrite.pImageInfo = NULL;       // Optional
  descriptorWrite.pTexelBufferView = NULL; // Optional

  vkUpdateDescriptorSets(VK.dev, 1, &descriptorWrite, 0, NULL);

  // Frame buffers for rendering
  for (uint32_t i = 0; i < WSI.vk.imgCount; i++) {
    VkImageView attachments[1] = {WSI.vk.swapImgView[i]};
    VkFramebufferCreateInfo framebufferInfo = {0};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = renderPass;
    framebufferInfo.attachmentCount = ARRAY_SIZEOF(attachments);
    framebufferInfo.pAttachments = attachments;
    framebufferInfo.width = swapSize().width;
    framebufferInfo.height = swapSize().height;
    framebufferInfo.layers = 1;

    result = vkCreateFramebuffer(VK.dev, &framebufferInfo, NULL, &WSI.vk.fb[i]);
    assert(result == VK_SUCCESS);
  }

  // Prepare command pools
  VkCommandPoolCreateInfo poolInfo = {0};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = VK.gfxIdx;
  result = vkCreateCommandPool(VK.dev, &poolInfo, NULL, &VK.cmdPool);
  assert(result == VK_SUCCESS);

  VkCommandBufferAllocateInfo bufAllocInfo = {0};
  bufAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  bufAllocInfo.commandPool = VK.cmdPool;
  bufAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  bufAllocInfo.commandBufferCount = 1;

  VkCommandBuffer commandBuffer;
  vkAllocateCommandBuffers(VK.dev, &bufAllocInfo, &commandBuffer);

  // Prepare sync objs
  VkSemaphoreCreateInfo semaphoreInfo = {0};
  semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  VkFenceCreateInfo fenceInfo = {0};
  fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  VkSemaphore imageAvailableSemaphore;
  VkSemaphore renderFinishedSemaphore;
  VkFence inFlightFence;
  assert(vkCreateSemaphore(VK.dev, &semaphoreInfo, NULL,
                           &imageAvailableSemaphore) == VK_SUCCESS);
  assert(vkCreateSemaphore(VK.dev, &semaphoreInfo, NULL,
                           &renderFinishedSemaphore) == VK_SUCCESS);
  assert(vkCreateFence(VK.dev, &fenceInfo, NULL, &inFlightFence) == VK_SUCCESS);

  // Begin drawing
  WSI.frame_done = true;
  float frame = 0;
  while (!WSI.window_closed && wl_display_dispatch_pending(WSI.display) != -1) {
    if (!WSI.frame_done)
      continue;

    frame += 1;
    vkWaitForFences(VK.dev, 1, &inFlightFence, VK_TRUE, UINT64_MAX);
    uint32_t imageIndex;
    result = vkAcquireNextImageKHR(VK.dev, WSI.vk.swapchain, UINT64_MAX,
                                   imageAvailableSemaphore, VK_NULL_HANDLE,
                                   &imageIndex);
    WSI.vk.recreate |= result == VK_ERROR_OUT_OF_DATE_KHR;

    // If the swapchain had to be recreated, also recreate pipelines's
    // framebuffers.
    if (WSI.vk.recreate) {
      WSI.vk.recreate = false;
      recreate_swapchain();
      // Frame buffers for rendering
      for (uint32_t i = 0; i < WSI.vk.imgCount; i++) {
        VkImageView attachments[1] = {WSI.vk.swapImgView[i]};
        VkFramebufferCreateInfo framebufferInfo = {0};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.attachmentCount = ARRAY_SIZEOF(attachments);
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = swapSize().width;
        framebufferInfo.height = swapSize().height;
        framebufferInfo.layers = 1;

        result =
            vkCreateFramebuffer(VK.dev, &framebufferInfo, NULL, &WSI.vk.fb[i]);
        assert(result == VK_SUCCESS);
      }
      // WSI might signal this, so dump this semaphore.
      vkDestroySemaphore(VK.dev, imageAvailableSemaphore, NULL);
      assert(vkCreateSemaphore(VK.dev, &semaphoreInfo, NULL,
                               &imageAvailableSemaphore) == VK_SUCCESS);
      continue;
    }
    // Assuming all is good we can reset it.
    vkResetFences(VK.dev, 1, &inFlightFence);

    // Begin recording rendering commands.
    // Depends on which framebuffer to use through RenderPassBegin

    VkCommandBufferBeginInfo beginInfo = {0};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    // Can implicitly reset the cmdbuffer later.
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkRenderPassBeginInfo renderPassBeginInfo = {0};
    renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassBeginInfo.renderPass = renderPass;
    renderPassBeginInfo.framebuffer = WSI.vk.fb[imageIndex];
    renderPassBeginInfo.renderArea.offset = (VkOffset2D){0, 0};
    renderPassBeginInfo.renderArea.extent = swapSize();
    VkClearValue clearColor = {{{0.2f, 0.4f, 0.9f, 1.0f}}};
    renderPassBeginInfo.clearValueCount = 1;
    renderPassBeginInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo,
                         VK_SUBPASS_CONTENTS_INLINE);

    // Add dynamic state
    VkViewport viewport = {0};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)swapSize().width;
    viewport.height = (float)swapSize().height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor = {0};
    scissor.offset = (VkOffset2D){0, 0};
    scissor.extent = swapSize();
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    float theta = frame * 3.1415f / 200.f;
    struct MData spin = {
        // clang-format off
        cosf(theta), -sinf(theta), 0.f, 0.f,
        sinf(theta), cosf(theta), 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 1.f,
        // clang-format on
    };
    memcpy(data, &spin, (size_t)matrixBuffer.ci.size);

    // Set the pipeline to draw through
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      graphicsPipeline);

    // Bind draw data
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertexBuffer.buf, offsets);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 0, 1, &descSet, 0, NULL);

    vkCmdDraw(commandBuffer, 3, 1, 0, 0);

    vkCmdEndRenderPass(commandBuffer);

    assert(vkEndCommandBuffer(commandBuffer) == VK_SUCCESS);

    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    // Waiting
    VkSemaphore waitSemaphores[] = {imageAvailableSemaphore};
    VkPipelineStageFlags waitStages[] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    // Doing
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    // Signaling
    VkSemaphore signalSemaphores[] = {renderFinishedSemaphore};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    // Begin drawing
    assert(vkQueueSubmit(VK.gfx, 1, &submitInfo, inFlightFence) == VK_SUCCESS);

    // Present.
    VkPresentInfoKHR presentInfo = {0};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    VkSwapchainKHR swapChains[1] = {WSI.vk.swapchain};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(VK.gfx, &presentInfo);
    WSI.vk.recreate |=
        (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR);
  }

  // Cleanup left to reader.

  return 0;
}
