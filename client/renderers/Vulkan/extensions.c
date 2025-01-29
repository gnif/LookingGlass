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

#include "extensions.h"

#include "common/array.h"
#include "common/debug.h"
#include "app.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static const char ** vulkan_checkExtensions(
    uint32_t extensionCount, VkExtensionProperties * extensions,
    size_t requiredExtensionCount, const char ** requiredExtensions,
    size_t optionalExtensionCount, const char ** optionalExtensions,
    uint32_t * enabledExtensionCount)
{
  *enabledExtensionCount = 0;

  size_t maxExtensionCount = requiredExtensionCount + optionalExtensionCount;
  const char **enabledExtensions = malloc(
      sizeof(const char *) * maxExtensionCount);

  for (uint32_t i = 0; i < requiredExtensionCount; ++i)
  {
    const char * extension = requiredExtensions[i];
    bool found = false;
    for (uint32_t j = 0; j < extensionCount; ++j)
    {
      if (!strcmp(extension, extensions[j].extensionName))
      {
        found = true;
        break;
      }
    }
    if (!found)
    {
      DEBUG_ERROR("Required extension '%s' is not supported", extension);
      goto err_enabled;
    }
    enabledExtensions[*enabledExtensionCount] = extension;
    ++*enabledExtensionCount;
  }

  for (uint32_t i = 0; i < optionalExtensionCount; ++i)
  {
    const char * extension = optionalExtensions[i];
    bool found = false;
    for (uint32_t j = 0; j < extensionCount; ++j)
    {
      if (!strcmp(extension, extensions[j].extensionName))
      {
        found = true;
        break;
      }
    }
    if (!found)
    {
      continue;
    }
    DEBUG_INFO("Enabling optional extension '%s'", extension);
    enabledExtensions[*enabledExtensionCount] = extension;
    ++*enabledExtensionCount;
  }

  return enabledExtensions;

err_enabled:
  free(enabledExtensions);
  *enabledExtensionCount = 0;

  return NULL;
}

const char ** vulkan_checkInstanceExtensions(uint32_t * enabledExtensionCount)
{
  uint32_t extensionCount;
  VkResult result = vkEnumerateInstanceExtensionProperties(NULL,
      &extensionCount, NULL);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to enumerate Vulkan instance extensions (VkResult: %d)",
        result);
    goto err;
  }

  struct VkExtensionProperties * extensions = malloc(
      sizeof(struct VkExtensionProperties) * extensionCount);
  if (!extensions)
  {
    DEBUG_ERROR("out of memory");
    goto err;
  }

  result = vkEnumerateInstanceExtensionProperties(NULL,
      &extensionCount, extensions);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to enumerate Vulkan instance extensions (VkResult: %d)",
        result);
    goto err_extensions;
  }

  const char * requiredExtensions[] =
  {
    "VK_KHR_surface",
    app_getVulkanSurfaceExtension()
  };

  const char * optionalExtensions[] =
  {
    "VK_EXT_swapchain_colorspace"
  };

  const char **enabledExtensions = vulkan_checkExtensions(extensionCount,
      extensions, ARRAY_LENGTH(requiredExtensions), requiredExtensions,
      ARRAY_LENGTH(optionalExtensions), optionalExtensions,
      enabledExtensionCount);

  free(extensions);
  return enabledExtensions;

err_extensions:
  free(extensions);

err:
  return NULL;
}

const char ** vulkan_checkDeviceExtensions(VkPhysicalDevice physicalDevice,
    uint32_t * enabledExtensionCount)
{
  uint32_t extensionCount;
  VkResult result = vkEnumerateDeviceExtensionProperties(physicalDevice,
      NULL, &extensionCount, NULL);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to enumerate Vulkan device extensions (VkResult: %d)",
        result);
    goto err;
  }

  struct VkExtensionProperties * extensions = malloc(
      sizeof(struct VkExtensionProperties) * extensionCount);
  if (!extensions)
  {
    DEBUG_ERROR("out of memory");
    goto err;
  }

  result = vkEnumerateDeviceExtensionProperties(physicalDevice, NULL,
      &extensionCount, extensions);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to enumerate Vulkan device extensions (VkResult: %d)",
        result);
    goto err_extensions;
  }

  const char * requiredExtensions[] =
  {
    "VK_KHR_swapchain"
  };

  const char * optionalExtensions[] =
  {
    "VK_EXT_hdr_metadata"
  };

  const char **enabledExtensions = vulkan_checkExtensions(extensionCount,
      extensions, ARRAY_LENGTH(requiredExtensions), requiredExtensions,
      ARRAY_LENGTH(optionalExtensions), optionalExtensions,
      enabledExtensionCount);

  free(extensions);
  return enabledExtensions;

err_extensions:
  free(extensions);

err:
  return NULL;
}
