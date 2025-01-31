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

#include "imgui.h"

#include "common/array.h"
#include "common/debug.h"
#include "common/vector.h"

#include <string.h>

#include "cimgui.h"
#include "generator/output/cimgui_impl.h"

#include "vulkan_util.h"

static VkResult vkResult;

typedef struct Texture
{
  VkImage         image;
  VkImageView     imageView;
  VkDeviceMemory  memory;
  VkDescriptorSet descriptorSet;
}
Texture;

struct Vulkan_ImGui
{
  VkInstance            instance;
  VkPhysicalDevice      physicalDevice;
  uint32_t              queueFamilyIndex;
  struct VkPhysicalDeviceMemoryProperties * memoryProperties;
  VkDevice              device;
  VkQueue               queue;
  VkCommandBuffer       commandBuffer;
  VkSampler             sampler;
  VkDescriptorPool      descriptorPool;
  VkFence               fence;

  VkDescriptorSetLayout descriptorSetLayout;
  Vector                textures;

  bool                  initialized;
};

static bool createDescriptorSetLayout(Vulkan_ImGui * this)
{
  struct VkDescriptorSetLayoutBinding bindings[] =
  {
    {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT
    },
  };

  struct VkDescriptorSetLayoutCreateInfo createInfo =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = ARRAY_LENGTH(bindings),
    .pBindings = bindings
  };

  VkResult result = vkCreateDescriptorSetLayout(this->device, &createInfo, NULL,
      &this->descriptorSetLayout);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to create descriptor set layout (VkResult: %d)",
        result);
    return false;
  }

  return true;
}

bool vulkan_imGuiInit(Vulkan_ImGui ** this, VkInstance instance,
    VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
    struct VkPhysicalDeviceMemoryProperties * memoryProperties, VkDevice device,
    VkQueue queue, VkCommandBuffer commandBuffer, VkSampler sampler,
    VkDescriptorPool descriptorPool, VkFence fence)
{
  *this = calloc(1, sizeof(Vulkan_ImGui));
  if (!*this)
  {
    DEBUG_ERROR("out of memory");
    goto err;
  }

  (*this)->instance = instance;
  (*this)->physicalDevice = physicalDevice;
  (*this)->queueFamilyIndex = queueFamilyIndex;
  (*this)->memoryProperties = memoryProperties;
  (*this)->device = device;
  (*this)->queue = queue;
  (*this)->commandBuffer = commandBuffer;
  (*this)->sampler = sampler;
  (*this)->descriptorPool = descriptorPool;
  (*this)->fence = fence;

  if (!createDescriptorSetLayout(*this))
    goto err_imgui;

  vector_create(&(*this)->textures, sizeof(Texture), 0);

  return true;

err_imgui:
  free(*this);
  *this = NULL;

err:
  return false;
}

static void freeTexture(Vulkan_ImGui * this, Texture * texture)
{
  vkDestroyImageView(this->device, texture->imageView, NULL);
  vkDestroyImage(this->device, texture->image, NULL);
  vkFreeMemory(this->device, texture->memory, NULL);
}

void vulkan_imGuiFree(Vulkan_ImGui ** imGui)
{
  Vulkan_ImGui * this = *imGui;
  if (!this)
    return;

  if (this->initialized)
    ImGui_ImplVulkan_Shutdown();

  if (this->descriptorSetLayout)
    vkDestroyDescriptorSetLayout(this->device, this->descriptorSetLayout, NULL);

  Texture * texture;
  vector_forEachRef(texture, &this->textures)
    freeTexture(this, texture);
  vector_destroy(&this->textures);

  free(this);
  *imGui = NULL;
}

static bool copyBufferToImage(Vulkan_ImGui * this, VkBuffer buffer,
    VkImage image, uint32_t width, uint32_t height)
{
  struct VkCommandBufferBeginInfo beginInfo =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
  };

  VkResult result = vkBeginCommandBuffer(this->commandBuffer, &beginInfo);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to begin command buffer (VkResult: %d)", result);
    return false;
  }

  struct VkImageMemoryBarrier preImageBarrier =
  {
    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_NONE,
    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .image = image,
    .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .subresourceRange.levelCount = 1,
    .subresourceRange.layerCount = 1
  };

  vkCmdPipelineBarrier(this->commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &preImageBarrier);

  struct VkBufferImageCopy region =
  {
    .imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .imageSubresource.layerCount = 1,
    .imageExtent.width = width,
    .imageExtent.height = height,
    .imageExtent.depth = 1
  };

  vkCmdCopyBufferToImage(this->commandBuffer, buffer, image,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  struct VkImageMemoryBarrier postImageBarrier =
  {
    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_NONE,
    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .image = image,
    .subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .subresourceRange.levelCount = 1,
    .subresourceRange.layerCount = 1
  };

  vkCmdPipelineBarrier(this->commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
      &postImageBarrier);

  result = vkEndCommandBuffer(this->commandBuffer);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to end command buffer (VkResult: %d)", result);
    return false;
  }

  struct VkSubmitInfo submitInfo =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1,
    .pCommandBuffers = &this->commandBuffer
  };

  result = vkQueueSubmit(this->queue, 1, &submitInfo, this->fence);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to submit command buffer (VkResult: %d)", result);
    goto err;
  }

  if (!vulkan_waitFence(this->device, this->fence))
    goto err_wait;

  return true;

err_wait:
  vkQueueWaitIdle(this->queue);

err:
  return false;
}

void * vulkan_imGuiCreateTexture(Vulkan_ImGui * this, int width, int height,
    uint8_t * data)
{
  struct VkImageCreateInfo createInfo =
  {
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .imageType = VK_IMAGE_TYPE_2D,
    .format = VK_FORMAT_R8G8B8A8_SRGB,
    .extent.width = width,
    .extent.height = height,
    .extent.depth = 1,
    .mipLevels = 1,
    .arrayLayers = 1,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .tiling = VK_IMAGE_TILING_OPTIMAL,
    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
  };

  VkImage image;
  VkResult result = vkCreateImage(this->device, &createInfo, NULL, &image);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to create image (VkResult: %d)", result);
    goto err;
  }

  struct VkMemoryRequirements memoryRequirements;
  vkGetImageMemoryRequirements(this->device, image, &memoryRequirements);
  uint32_t memoryTypeIndex = vulkan_findMemoryType(this->memoryProperties,
      memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (memoryTypeIndex == UINT32_MAX)
  {
    DEBUG_ERROR("Could not find suitable memory type for image");
    goto err_image;
  }

  struct VkMemoryAllocateInfo allocateInfo =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = memoryRequirements.size,
    .memoryTypeIndex = memoryTypeIndex
  };

  VkDeviceMemory memory;
  result = vkAllocateMemory(this->device, &allocateInfo, NULL, &memory);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to allocate image memory (VkResult: %d)", result);
    goto err_image;
  }

  result = vkBindImageMemory(this->device, image, memory, 0);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to bind image memory (VkResult: %d)", result);
    goto err_memory;
  }

  VkImageView imageView = vulkan_createImageView(this->device, image,
      createInfo.format);
  if (!imageView)
    goto err_memory;

  VkDeviceMemory stagingMemory;
  void *stagingMap;
  VkBuffer stagingBuffer = vulkan_createBuffer(this->memoryProperties,
      this->device, width * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      &stagingMemory, &stagingMap);
  if (!stagingBuffer)
    goto err_image_view;

  memcpy(stagingMap, data, width * height * 4);

  if (!copyBufferToImage(this, stagingBuffer, image, width, height))
    goto err_staging;

  VkDescriptorSet descriptorSet = vulkan_allocateDescriptorSet(this->device,
      this->descriptorSetLayout, this->descriptorPool);
  if (!descriptorSet)
    goto err_staging;

  struct VkDescriptorImageInfo imageInfo =
  {
    .sampler = this->sampler,
    .imageView = imageView,
    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
  };

  struct VkWriteDescriptorSet descriptorWrites[] =
  {
    {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = descriptorSet,
      .dstBinding = 0,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
      .pImageInfo = &imageInfo
    }
  };

  vkUpdateDescriptorSets(this->device, ARRAY_LENGTH(descriptorWrites),
      descriptorWrites, 0, NULL);

  Texture texture =
  {
    .image = image,
    .imageView = imageView,
    .memory = memory,
    .descriptorSet = descriptorSet
  };

  if (!vector_push(&this->textures, &texture))
    goto err_staging;

  vkUnmapMemory(this->device, stagingMemory);
  vkDestroyBuffer(this->device, stagingBuffer, NULL);
  vkFreeMemory(this->device, stagingMemory, NULL);
  return descriptorSet;

err_staging:
  vkUnmapMemory(this->device, stagingMemory);
  vkDestroyBuffer(this->device, stagingBuffer, NULL);
  vkFreeMemory(this->device, stagingMemory, NULL);

err_image_view:
  vkDestroyImageView(this->device, imageView, NULL);

err_memory:
  vkFreeMemory(this->device, memory, NULL);

err_image:
  vkDestroyImage(this->device, image, NULL);

err:
  return false;
}

void vulkan_imGuiFreeTexture(Vulkan_ImGui * this, void * texture)
{
  VkDescriptorSet descriptorSet = texture;

  Texture * imGuiTexture;
  vector_forEachRefIdx(i, imGuiTexture, &this->textures)
  {
    if (imGuiTexture->descriptorSet == descriptorSet)
    {
      freeTexture(this, imGuiTexture);
      vector_remove(&this->textures, i);
      return;
    }
  }
  DEBUG_FATAL("Unknown texture");
}

static void checkVkResult(VkResult result)
{
  if (result != VK_SUCCESS)
    vkResult = result;
}

bool vulkan_imGuiInitPipeline(Vulkan_ImGui * this, uint32_t swapchainImageCount,
    VkRenderPass renderPass)
{
  if (this->initialized)
  {
    ImGui_ImplVulkan_Shutdown();
    this->initialized = false;
  }

  struct ImGui_ImplVulkan_InitInfo initInfo =
  {
    .Instance = this->instance,
    .PhysicalDevice = this->physicalDevice,
    .Device = this->device,
    .QueueFamily = this->queueFamilyIndex,
    .Queue = this->queue,
    .DescriptorPool = this->descriptorPool,
    .RenderPass = renderPass,
    .MinImageCount = swapchainImageCount,
    .ImageCount = swapchainImageCount,
    .MSAASamples = VK_SAMPLE_COUNT_1_BIT,
    .CheckVkResultFn = checkVkResult
  };

  vkResult = VK_SUCCESS;
  if (!ImGui_ImplVulkan_Init(&initInfo) || vkResult != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to initialize ImGui");
    return false;
  }

  this->initialized = true;
  return true;
}

void vulkan_imGuiDeinitPipeline(Vulkan_ImGui * this)
{
  if (this->initialized)
  {
    ImGui_ImplVulkan_Shutdown();
    this->initialized = false;
  }
}

bool vulkan_imGuiUploadFonts(Vulkan_ImGui * this)
{
  vkResult = VK_SUCCESS;
  if (!ImGui_ImplVulkan_CreateFontsTexture() || vkResult != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to create ImGui fonts texture (VkResult: %d)",
        vkResult);
    return false;
  }

  return true;
}

bool vulkan_imGuiRecordCommandBuffer(Vulkan_ImGui * this)
{
  vkResult = VK_SUCCESS;
  ImGui_ImplVulkan_NewFrame();
  if (vkResult != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to start ImGui frame (VkResult: %d)", vkResult);
    return false;
  }

  vkResult = VK_SUCCESS;
  ImGui_ImplVulkan_RenderDrawData(igGetDrawData(), this->commandBuffer, NULL);
  if (vkResult != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to render ImGui (VkResult: %d)", vkResult);
    return false;
  }

  return true;
}
