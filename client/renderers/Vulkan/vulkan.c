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

struct Inst
{
  LG_Renderer base;

  VkInstance instance;
  VkSurfaceKHR surface;
  VkDevice device;
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

static void vulkan_deinitialize(LG_Renderer * renderer)
{
  struct Inst * this = UPCAST(struct Inst, renderer);

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
  DEBUG_FATAL("vulkan_supports not implemented");
}

static void vulkan_onRestart(LG_Renderer * renderer)
{
}

static void vulkan_onResize(LG_Renderer * renderer, const int width, const int height, const double scale,
    const LG_RendererRect destRect, LG_RendererRotate rotate)
{
  DEBUG_FATAL("vulkan_onResize not implemented");
}

static bool vulkan_onMouseShape(LG_Renderer * renderer, const LG_RendererCursor cursor,
    const int width, const int height,
    const int pitch, const uint8_t * data)
{
  DEBUG_FATAL("vulkan_onMouseShape not implemented");
}

static bool vulkan_onMouseEvent(LG_Renderer * renderer, const bool visible,
    int x, int y, const int hx, const int hy)
{
  DEBUG_FATAL("vulkan_onMouseEvent not implemented");
}

static bool vulkan_onFrameFormat(LG_Renderer * renderer, const LG_RendererFormat format)
{
  DEBUG_FATAL("vulkan_onFrameFormat not implemented");
}

static bool vulkan_onFrame(LG_Renderer * renderer, const FrameBuffer * frame, int dmaFd,
    const FrameDamageRect * damageRects, int damageRectsCount)
{
  DEBUG_FATAL("vulkan_onFrame not implemented");
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
  VkPhysicalDevice physicalDevice = vulkan_pickPhysicalDevice(this->instance,
      &queueFamilyIndex);
  if (!physicalDevice)
    goto err_surf;

  this->device = vulkan_createDevice(this->instance, physicalDevice,
      queueFamilyIndex);
  if (!this->device)
    goto err_surf;

  DEBUG_FATAL("vulkan_renderStartup not implemented");

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
  DEBUG_FATAL("vulkan_render not implemented");
}

static void * vulkan_createTexture(LG_Renderer * renderer,
  int width, int height, uint8_t * data)
{
  DEBUG_FATAL("vulkan_createTexture not implemented");
}

static void vulkan_freeTexture(LG_Renderer * renderer, void * texture)
{
  DEBUG_FATAL("vulkan_freeTexture not implemented");
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
