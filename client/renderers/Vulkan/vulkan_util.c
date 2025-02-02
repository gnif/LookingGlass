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

#include "vulkan_util.h"

#include "common/debug.h"

uint32_t vulkan_findMemoryType(
    struct VkPhysicalDeviceMemoryProperties * memoryProperties,
    uint32_t memoryTypeBits, VkMemoryPropertyFlags requiredProperties)
{
  for (uint32_t i = 0; i < memoryProperties->memoryTypeCount; ++i)
  {
    if ((memoryTypeBits & (1 << i)) == 0)
      continue;

    VkMemoryPropertyFlags properties =
        memoryProperties->memoryTypes[i].propertyFlags;
    if ((properties & requiredProperties) == requiredProperties)
      return i;
  }

  return UINT32_MAX;
}

VkDeviceMemory vulkan_allocateMemory(
    struct VkPhysicalDeviceMemoryProperties *memoryProperties,
    VkDevice device, struct VkMemoryRequirements *memoryRequirements,
    VkMemoryPropertyFlags requiredProperties)
{
  uint32_t memoryTypeIndex = vulkan_findMemoryType(memoryProperties,
      memoryRequirements->memoryTypeBits, requiredProperties);
  if (memoryTypeIndex == UINT32_MAX)
  {
    DEBUG_ERROR("Could not find suitable memory type with properties %"PRIu32,
        requiredProperties);
    return NULL;
  }

  struct VkMemoryAllocateInfo allocateInfo =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = memoryRequirements->size,
    .memoryTypeIndex = memoryTypeIndex
  };

  VkDeviceMemory memory;
  VkResult result = vkAllocateMemory(device, &allocateInfo, NULL, &memory);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to allocate memory (VkResult: %d)", result);
    return NULL;
  }

  return memory;
}

VkDescriptorSet vulkan_allocateDescriptorSet(VkDevice device,
    VkDescriptorSetLayout layout, VkDescriptorPool descriptorPool)
{
  struct VkDescriptorSetAllocateInfo allocateInfo =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = descriptorPool,
    .descriptorSetCount = 1,
    .pSetLayouts = &layout
  };

  VkDescriptorSet descriptorSet;
  VkResult result = vkAllocateDescriptorSets(device, &allocateInfo,
      &descriptorSet);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to allocate descriptor set (VkResult: %d)", result);
    return NULL;
  }

  return descriptorSet;
}

VkBuffer vulkan_createBuffer(
    struct VkPhysicalDeviceMemoryProperties * memoryProperties, VkDevice device,
    VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory * memory,
    void * map)
{
  struct VkBufferCreateInfo createInfo =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = size,
    .usage = usage,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE
  };

  VkBuffer buffer;
  VkResult result = vkCreateBuffer(device, &createInfo, NULL, &buffer);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to create buffer (VkResult: %d)", result);
    goto err;
  }

  struct VkMemoryRequirements memoryRequirements;
  vkGetBufferMemoryRequirements(device, buffer, &memoryRequirements);
  uint32_t memoryTypeIndex = vulkan_findMemoryType(memoryProperties,
      memoryRequirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (memoryTypeIndex == UINT32_MAX)
  {
    DEBUG_ERROR("Could not find suitable memory type for buffer");
    goto err_buffer;
  }

  struct VkMemoryAllocateInfo allocateInfo =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = memoryRequirements.size,
    .memoryTypeIndex = memoryTypeIndex
  };

  result = vkAllocateMemory(device, &allocateInfo, NULL, memory);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to allocate buffer memory (VkResult: %d)", result);
    goto err_buffer;
  }

  result = vkBindBufferMemory(device, buffer, *memory, 0);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to bind buffer memory (VkResult: %d)",
        result);
    goto err_memory;
  }

  result = vkMapMemory(device, *memory, 0, VK_WHOLE_SIZE, 0, map);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to map buffer memory (VkResult: %d)", result);
    goto err_memory;
  }

  return buffer;

err_memory:
  vkFreeMemory(device, *memory, NULL);
  *memory = NULL;

err_buffer:
  vkDestroyBuffer(device, buffer, NULL);

err:
  return false;
}

VkImageView vulkan_createImageView(VkDevice device, VkImage image,
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

bool vulkan_waitFence(VkDevice device, VkFence fence)
{
  VkResult result = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to wait for fence (VkResult: %d)", result);
    return false;
  }

  result = vkResetFences(device, 1, &fence);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to reset fence (VkResult: %d)", result);
    return false;
  }

  return true;
}
