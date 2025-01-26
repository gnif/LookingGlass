/**
 * Looking Glass
 * Copyright © 2017-2024 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "interface/renderer.h"

#include "common/array.h"

#include <vulkan/vulkan.h>

#include <string.h>

struct Inst
{
  LG_Renderer base;

  VkInstance       instance;
  VkSurfaceKHR     surface;
  VkPhysicalDevice physicalDevice;
  VkDevice         device;

  VkSwapchainKHR   swapchain;
  VkFormat         swapchainFormat;
  VkExtent2D       swapchainExtent;
  uint32_t         swapchainImageCount;
  VkImageView *    swapchainImageViews;
  VkRenderPass     renderPass;

  LG_RendererFormat format;

  int               width, height;
};

static const char * vulkan_getName(void)
{
  return "Vulkan";
}

static void vulkan_setup(void)
{
}

static bool vulkan_create(LG_Renderer ** renderer, const LG_RendererParams params,
    bool * needsOpenGL)
{
  struct Inst * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_INFO("Failed to allocate %lu bytes", sizeof(*this));
    return false;
  }
  *renderer = &this->base;

  *needsOpenGL = false;
  return true;
}

static bool vulkan_initialize(LG_Renderer * renderer)
{
  return true;
}

static void vulkan_freeSwapchain(struct Inst * this)
{
  if (this->swapchainImageViews)
  {
    for (uint32_t i = 0; i < this->swapchainImageCount; ++i)
    {
      if (this->swapchainImageViews[i])
        vkDestroyImageView(this->device, this->swapchainImageViews[i], NULL);
    }
    free(this->swapchainImageViews);
    this->swapchainImageViews = NULL;
  }

  if (this->swapchain)
  {
    vkDestroySwapchainKHR(this->device, this->swapchain, NULL);
    this->swapchain = NULL;
    this->swapchainFormat = VK_FORMAT_UNDEFINED;
    this->swapchainExtent.width = 0;
    this->swapchainExtent.height = 0;
  }
}

static void vulkan_deinitialize(LG_Renderer * renderer)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  if (this->renderPass)
    vkDestroyRenderPass(this->device, this->renderPass, NULL);

  vulkan_freeSwapchain(this);

  if (this->device)
    vkDestroyDevice(this->device, NULL);

  if (this->surface)
    vkDestroySurfaceKHR(this->instance, this->surface, NULL);

  if (this->instance)
    vkDestroyInstance(this->instance, NULL);

  free(this);
}

static bool vulkan_supports(LG_Renderer * renderer, LG_RendererSupport flag)
{
  return false;
}

static void vulkan_onRestart(LG_Renderer * renderer)
{
}

static VkImageView vulkan_createImageView(VkDevice device, VkImage image,
    VkFormat format)
{
  struct VkImageViewCreateInfo createInfo =
  {
    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
    .image = image,
    .viewType = VK_IMAGE_VIEW_TYPE_2D,
    .format = format,
    .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .subresourceRange.levelCount = 1,
    .subresourceRange.layerCount = 1
  };

  VkImageView imageView;
  VkResult result = vkCreateImageView(device, &createInfo, NULL, &imageView);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to create image view (VkResult: %d)", result);
    return NULL;
  }

  return imageView;
}

static bool vulkan_getSwapchainImages(struct Inst * this)
{
  uint32_t imageCount;
  VkResult result = vkGetSwapchainImagesKHR(this->device, this->swapchain,
      &imageCount, NULL);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to get swapchain images (VkResult: %d)", result);
    goto err;
  }

  VkImage * images = malloc(sizeof(VkImage) * imageCount);
  if (!images)
  {
    DEBUG_ERROR("out of memory");
    goto err;
  }

  result = vkGetSwapchainImagesKHR(this->device, this->swapchain, &imageCount,
      images);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to get swapchain images (VkResult: %d)", result);
    goto err_images;
  }

  VkImageView * imageViews = calloc(imageCount, sizeof(VkImageView));
  if (!imageViews)
  {
    DEBUG_ERROR("out of memory");
    goto err_images;
  }

  for (uint32_t i = 0; i < imageCount; ++i)
  {
    imageViews[i] = vulkan_createImageView(this->device, images[i],
        this->swapchainFormat);
    if (!imageViews[i]) {
      goto err_image_views;
    }
  }

  free(images);
  this->swapchainImageCount = imageCount;
  this->swapchainImageViews = imageViews;
  return true;

err_image_views:
  for (uint32_t i = 0; i < imageCount; ++i)
  {
    if (imageViews[i])
      vkDestroyImageView(this->device, imageViews[i], NULL);
  }
  free(imageViews);

err_images:
  free(images);

err:
  return false;
}

static VkSurfaceFormatKHR vulkan_selectSurfaceFormat(struct Inst * this,
    FrameType frameType)
{
  struct VkSurfaceFormatKHR format =
  {
    .format = VK_FORMAT_UNDEFINED
  };

  uint32_t formatCount;
  VkResult result = vkGetPhysicalDeviceSurfaceFormatsKHR(this->physicalDevice,
      this->surface, &formatCount, NULL);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to get surface formats (VkResult: %d)", result);
    goto err;
  }

  VkSurfaceFormatKHR * formats = malloc(
      sizeof(VkSurfaceFormatKHR) * formatCount);
  if (!formats)
  {
    DEBUG_ERROR("out of memory");
    goto err;
  }

  result = vkGetPhysicalDeviceSurfaceFormatsKHR(this->physicalDevice,
      this->surface, &formatCount, formats);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to get surface formats (VkResult: %d)", result);
    goto err_formats;
  }

  // TODO: Handle 10-bit HDR
  uint32_t formatIndex = UINT32_MAX;
  if (frameType == FRAME_TYPE_RGBA16F)
  {
    for (uint32_t i = 0; i < formatCount; ++i)
    {
      if (formats[i].format == VK_FORMAT_R16G16B16A16_SFLOAT &&
          formats[i].colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)
      {
        formatIndex = i;
        break;
      }
    }
    if (formatIndex == UINT32_MAX)
    {
      DEBUG_WARN("Could not find suitable 16-bit surface format; HDR content will look bad");
    }
  }

  if (formatIndex == UINT32_MAX)
  {
    for (uint32_t i = 0; i < formatCount; ++i)
    {
      if ((formats[i].format == VK_FORMAT_R8G8B8A8_UNORM || formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) &&
          formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      {
        formatIndex = i;
        break;
      }
    }
  }

  if (formatIndex == UINT32_MAX)
  {
    DEBUG_ERROR("Could not find any suitable surface format");
    goto err_formats;
  }

  format = formats[formatIndex];
  free(formats);
  return format;

err_formats:
  free(formats);

err:
  return format;
}

static bool vulkan_createSwapchain(struct Inst * this,
    VkSurfaceFormatKHR surfaceFormat)
{
  vulkan_freeSwapchain(this);

  VkSurfaceCapabilitiesKHR surfaceCaps;
  VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
      this->physicalDevice, this->surface, &surfaceCaps);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to get surface capabilities (VkResult: %d)", result);
    goto err;
  }

  VkCompositeAlphaFlagBitsKHR compositeAlpha;
  if (surfaceCaps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
    compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  else if (surfaceCaps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
    compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
  else if (surfaceCaps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
    compositeAlpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
  else if (surfaceCaps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
    compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  else
    DEBUG_FATAL("No supported composite alpha mode");

  uint32_t modeCount;
  result = vkGetPhysicalDeviceSurfacePresentModesKHR(this->physicalDevice,
      this->surface, &modeCount, NULL);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to get surface present modes (VkResult: %d)", result);
    goto err;
  }

  VkPresentModeKHR * modes = malloc(sizeof(VkPresentModeKHR) * modeCount);
  if (!modes)
  {
    DEBUG_ERROR("out of memory");
    goto err;
  }

  result = vkGetPhysicalDeviceSurfacePresentModesKHR(this->physicalDevice,
      this->surface, &modeCount, modes);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to get surface present modes (VkResult: %d)", result);
    goto err_modes;
  }

  VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
  for (uint32_t i = 0; i < modeCount; ++i)
  {
    if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
    {
      presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
      break;
    }
  }

  struct VkSwapchainCreateInfoKHR createInfo =
  {
    .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
    .surface = this->surface,
    .minImageCount = surfaceCaps.minImageCount,
    .imageFormat = surfaceFormat.format,
    .imageColorSpace = surfaceFormat.colorSpace,
    .imageExtent.width = this->width,
    .imageExtent.height = this->height,
    .imageArrayLayers = 1,
    .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
    .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
    .preTransform = surfaceCaps.currentTransform,
    .compositeAlpha = compositeAlpha,
    .presentMode = presentMode,
    .clipped = VK_TRUE
  };

  DEBUG_INFO("Min image count: %"PRIu32, createInfo.minImageCount);
  DEBUG_INFO("Image format   : %d", createInfo.imageFormat);
  DEBUG_INFO("Color space    : %d", createInfo.imageColorSpace);
  DEBUG_INFO("Extent         : %"PRIu32"x%"PRIu32,
      createInfo.imageExtent.width, createInfo.imageExtent.height);
  DEBUG_INFO("Pre-transform  : %d", createInfo.preTransform);
  DEBUG_INFO("Composite alpha: %d", createInfo.compositeAlpha);
  DEBUG_INFO("Present mode   : %d", createInfo.presentMode);

  result = vkCreateSwapchainKHR(this->device, &createInfo, NULL,
      &this->swapchain);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to create swapchain (VkResult: %d)", result);
    goto err_modes;
  }
  this->swapchainFormat = createInfo.imageFormat;
  this->swapchainExtent = createInfo.imageExtent;

  free(modes);
  return true;

err_modes:
  free(modes);

err:
  return false;
}

static bool vulkan_createRenderPass(struct Inst * this)
{
  if (this->renderPass)
  {
    vkDestroyRenderPass(this->device, this->renderPass, NULL);
    this->renderPass = NULL;
  }

  struct VkAttachmentDescription attachment =
  {
    .format = this->swapchainFormat,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
  };

  struct VkAttachmentReference colorAttachment =
  {
    .attachment = 0,
    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
  };

  struct VkSubpassDescription subpass =
  {
    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
    .colorAttachmentCount = 1,
    .pColorAttachments = &colorAttachment
  };

  struct VkSubpassDependency dependency =
  {
    .srcSubpass = VK_SUBPASS_EXTERNAL,
    .dstSubpass = 0,
    .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
  };

  struct VkRenderPassCreateInfo createInfo =
  {
    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
    .attachmentCount = 1,
    .pAttachments = &attachment,
    .subpassCount = 1,
    .pSubpasses = &subpass,
    .dependencyCount = 1,
    .pDependencies = &dependency
  };

  VkResult result = vkCreateRenderPass(this->device, &createInfo, NULL,
      &this->renderPass);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to create render pass (VkResult: %d)", result);
    return false;
  }

  return true;
}

static bool vulkan_initPipeline(struct Inst * this)
{
  VkSurfaceFormatKHR surfaceFormat = vulkan_selectSurfaceFormat(this,
      this->format.type);
  if (surfaceFormat.format == VK_FORMAT_UNDEFINED)
    goto err;

  if (this->width != this->swapchainExtent.width ||
      this->height != this->swapchainExtent.height ||
      surfaceFormat.format != this->swapchainFormat)
  {
    if (!vulkan_createSwapchain(this, surfaceFormat))
      goto err;

    if (!vulkan_getSwapchainImages(this))
      goto err_swapchain;

    if (!vulkan_createRenderPass(this))
      goto err_swapchain;
  }

  return true;

err_swapchain:
  vulkan_freeSwapchain(this);

err:
  return false;
}

static bool vulkan_onResize(LG_Renderer * renderer, const int width,
    const int height, const double scale, const LG_RendererRect destRect,
    LG_RendererRotate rotate)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  this->width   = width * scale;
  this->height  = height * scale;

  if (!vulkan_initPipeline(this))
    return false;

  return true;
}

static bool vulkan_onMouseShape(LG_Renderer * renderer, const LG_RendererCursor cursor,
    const int width, const int height,
    const int pitch, const uint8_t * data)
{
  DEBUG_ERROR("vulkan_onMouseShape not implemented");
  return true;
}

static bool vulkan_onMouseEvent(LG_Renderer * renderer, const bool visible,
    int x, int y, const int hx, const int hy)
{
  DEBUG_ERROR("vulkan_onMouseEvent not implemented");
  return true;
}

static bool vulkan_onFrameFormat(LG_Renderer * renderer,
    const LG_RendererFormat format)
{
  struct Inst * this = UPCAST(struct Inst, renderer);
  memcpy(&this->format, &format, sizeof(LG_RendererFormat));

  if (!vulkan_initPipeline(this))
    goto err;

  return true;

err:
  return false;
}

static bool vulkan_onFrame(LG_Renderer * renderer, const FrameBuffer * frame,
    int dmaFd, const FrameDamageRect * damageRects, int damageRectsCount)
{
  DEBUG_ERROR("vulkan_onFrame not implemented");
  return true;
}

static VkInstance vulkan_createInstance(void)
{
  struct VkApplicationInfo appInfo =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "Looking Glass",
    .apiVersion = VK_API_VERSION_1_0
  };

  const char * extensions[] =
  {
    "VK_EXT_swapchain_colorspace",
    "VK_KHR_surface",
    "VK_KHR_wayland_surface",
    "VK_KHR_xlib_surface"
  };

  struct VkInstanceCreateInfo createInfo =
  {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &appInfo,
    .enabledExtensionCount = ARRAY_LENGTH(extensions),
    .ppEnabledExtensionNames = extensions
  };

  VkInstance instance;
  VkResult result = vkCreateInstance(&createInfo, NULL, &instance);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to create Vulkan instance (VkResult: %d)", result);
    return NULL;
  }

  return instance;
}

static VkPhysicalDevice vulkan_pickPhysicalDevice(VkInstance instance,
    uint32_t * queueFamilyIndex)
{
  uint32_t deviceCount;
  VkResult result = vkEnumeratePhysicalDevices(instance, &deviceCount, NULL);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to enumerate physical devices (VkResult: %d)", result);
    goto err;
  }
  if (deviceCount == 0)
  {
    DEBUG_ERROR("No Vulkan devices available");
    goto err;
  }

  VkPhysicalDevice * devices = malloc(sizeof(VkPhysicalDevice) * deviceCount);
  if (!devices)
  {
    DEBUG_ERROR("out of memory");
    goto err;
  }

  result = vkEnumeratePhysicalDevices(instance, &deviceCount, devices);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to enumerate physical devices (VkResult: %d)", result);
    goto err_devices;
  }

  VkPhysicalDevice device = NULL;
  uint32_t queueFamily = UINT32_MAX;
  for (uint32_t i = 0; i < deviceCount; ++i)
  {
    uint32_t queueFamilyCount;
    vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queueFamilyCount,
        NULL);

    VkQueueFamilyProperties * queueFamilyProperties = malloc(
        sizeof(VkQueueFamilyProperties) * queueFamilyCount);
    if (!queueFamilyProperties)
    {
      DEBUG_ERROR("out of memory");
      goto err_devices;
    }

    vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &queueFamilyCount,
        queueFamilyProperties);

    queueFamily = UINT32_MAX;
    for (uint32_t j = 0; j < queueFamilyCount; ++j)
    {
      if (queueFamilyProperties[j].queueFlags & VK_QUEUE_GRAPHICS_BIT)
      {
        queueFamily = j;
        break;
      }
    }
    free(queueFamilyProperties);
    if (queueFamily == UINT32_MAX)
      continue;

    device = devices[i];
    break;
  }
  if (device == NULL)
  {
    DEBUG_ERROR("Could not find any usable Vulkan device");
    goto err_devices;
  }

  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(device, &properties);

  DEBUG_INFO("Device      : %s", properties.deviceName);
  DEBUG_INFO("Queue family: %"PRIu32, queueFamily);

  free(devices);
  *queueFamilyIndex = queueFamily;
  return device;

err_devices:
  free(devices);

err:
  return NULL;
}

static VkDevice vulkan_createDevice(VkInstance instance,
    VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex)
{
  float queuePriority = 1.0f;

  struct VkDeviceQueueCreateInfo queueCreateInfo =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = queueFamilyIndex,
    .queueCount = 1,
    .pQueuePriorities = &queuePriority
  };

  const char * extensions[] =
  {
    "VK_KHR_swapchain"
  };

  struct VkDeviceCreateInfo createInfo =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .queueCreateInfoCount = 1,
    .pQueueCreateInfos = &queueCreateInfo,
    .enabledExtensionCount = ARRAY_LENGTH(extensions),
    .ppEnabledExtensionNames = extensions
  };

  VkDevice device;
  VkResult result = vkCreateDevice(physicalDevice, &createInfo, NULL, &device);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to create Vulkan device (VkResult: %d)", result);
    return NULL;
  }

  return device;
}

static bool vulkan_renderStartup(LG_Renderer * renderer, bool useDMA)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

  this->instance = vulkan_createInstance();
  if (!this->instance)
    goto err;

  this->surface = app_createVulkanSurface(this->instance);
  if (!this->surface)
    goto err_inst;

  uint32_t queueFamilyIndex;
  this->physicalDevice = vulkan_pickPhysicalDevice(this->instance,
      &queueFamilyIndex);
  if (!this->physicalDevice)
    goto err_surf;

  this->device = vulkan_createDevice(this->instance, this->physicalDevice,
      queueFamilyIndex);
  if (!this->device)
    goto err_surf;

  return true;

err_surf:
  vkDestroySurfaceKHR(this->instance, this->surface, NULL);
  this->surface = NULL;

err_inst:
  vkDestroyInstance(this->instance, NULL);
  this->instance = NULL;

err:
  return false;
}

static bool vulkan_render(LG_Renderer * renderer, LG_RendererRotate rotate,
    const bool newFrame, const bool invalidateWindow,
    void (*preSwap)(void * udata), void * udata)
{
  DEBUG_ERROR("vulkan_render not implemented");
  return true;
}

static void * vulkan_createTexture(LG_Renderer * renderer,
  int width, int height, uint8_t * data)
{
  DEBUG_ERROR("vulkan_createTexture not implemented");
  return (void *) 1;
}

static void vulkan_freeTexture(LG_Renderer * renderer, void * texture)
{
  DEBUG_ERROR("vulkan_freeTexture not implemented");
}

static void vulkan_spiceConfigure(LG_Renderer * renderer, int width, int height)
{
  DEBUG_FATAL("vulkan_spiceConfigure not implemented");
}

static void vulkan_spiceDrawFill(LG_Renderer * renderer, int x, int y, int width,
    int height, uint32_t color)
{
  DEBUG_FATAL("vulkan_spiceDrawFill not implemented");
}

static void vulkan_spiceDrawBitmap(LG_Renderer * renderer, int x, int y, int width,
    int height, int stride, uint8_t * data, bool topDown)
{
  DEBUG_FATAL("vulkan_spiceDrawBitmap not implemented");
}

static void vulkan_spiceShow(LG_Renderer * renderer, bool show)
{
  DEBUG_FATAL("vulkan_spiceShow not implemented");
}

struct LG_RendererOps LGR_Vulkan =
{
  .getName       = vulkan_getName,
  .setup         = vulkan_setup,
  .create        = vulkan_create,
  .initialize    = vulkan_initialize,
  .deinitialize  = vulkan_deinitialize,
  .supports      = vulkan_supports,
  .onRestart     = vulkan_onRestart,
  .onResize      = vulkan_onResize,
  .onMouseShape  = vulkan_onMouseShape,
  .onMouseEvent  = vulkan_onMouseEvent,
  .onFrameFormat = vulkan_onFrameFormat,
  .onFrame       = vulkan_onFrame,
  .renderStartup = vulkan_renderStartup,
  .render        = vulkan_render,
  .createTexture = vulkan_createTexture,
  .freeTexture   = vulkan_freeTexture,

  .spiceConfigure  = vulkan_spiceConfigure,
  .spiceDrawFill   = vulkan_spiceDrawFill,
  .spiceDrawBitmap = vulkan_spiceDrawBitmap,
  .spiceShow       = vulkan_spiceShow
};
