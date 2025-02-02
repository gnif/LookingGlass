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

#pragma once

#include <vulkan/vulkan.h>

#include <stdbool.h>
#include <stdint.h>

uint32_t vulkan_findMemoryType(
    struct VkPhysicalDeviceMemoryProperties * memoryProperties,
    uint32_t memoryTypeBits, VkMemoryPropertyFlags requiredProperties);

VkDeviceMemory vulkan_allocateMemory(
    struct VkPhysicalDeviceMemoryProperties * memoryProperties,
    VkDevice device, struct VkMemoryRequirements *memoryRequirements,
    VkMemoryPropertyFlags requiredProperties);

VkDescriptorSet vulkan_allocateDescriptorSet(VkDevice device,
    VkDescriptorSetLayout layout, VkDescriptorPool descriptorPool);

VkBuffer vulkan_createBuffer(
    struct VkPhysicalDeviceMemoryProperties * memoryProperties, VkDevice device,
    VkDeviceSize size, VkBufferUsageFlags usage, VkDeviceMemory * memory,
    void * map);

VkImageView vulkan_createImageView(VkDevice device, VkImage image,
    VkFormat format);

bool vulkan_waitFence(VkDevice device, VkFence fence);
