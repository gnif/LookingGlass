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

#include "interface/renderer.h"

typedef struct Vulkan_Desktop Vulkan_Desktop;

bool vulkan_desktopInit(Vulkan_Desktop ** desktop,
    struct VkPhysicalDeviceMemoryProperties * memoryProperties, VkDevice device,
    VkQueue queue, VkShaderModule vertexShader, VkCommandBuffer commandBuffer,
    VkDescriptorSetLayout descriptorSetLayout, VkDescriptorPool descriptorPool,
    VkPipelineLayout pipelineLayout, VkFence fence);
void vulkan_desktopFree(Vulkan_Desktop ** desktop);

bool vulkan_desktopInitPipeline(Vulkan_Desktop * this, VkRenderPass renderPass);
bool vulkan_desktopInitFormat(Vulkan_Desktop * this,
    const LG_RendererFormat * format);

bool vulkan_desktopUpdate(Vulkan_Desktop * this, const FrameBuffer * frame);
bool vulkan_desktopRender(Vulkan_Desktop * this, float translateX,
    float translateY, float scaleX, float scaleY, LG_RendererRotate rotate,
    float whiteLevel);
