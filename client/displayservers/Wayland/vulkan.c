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

#ifdef ENABLE_VULKAN
#include "wayland.h"

#include <vulkan/vulkan_wayland.h>

const char * waylandGetVulkanSurfaceExtension(void)
{
  return "VK_KHR_wayland_surface";
}

VkSurfaceKHR waylandCreateVulkanSurface(VkInstance instance)
{
  struct VkWaylandSurfaceCreateInfoKHR createInfo =
  {
    .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
    .display = wlWm.display,
    .surface = wlWm.surface
  };

  VkSurfaceKHR surface;
  VkResult result = vkCreateWaylandSurfaceKHR(instance, &createInfo, NULL,
      &surface);
  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to create Vulkan Wayland surface (VkResult: %d)",
        result);
    return NULL;
  }

  return surface;
}

bool waylandVulkanPresent(VkQueue queue, struct VkPresentInfoKHR * presentInfo)
{
  waylandPresentationFrame();

  // vkQueuePresentKHR issues a batch of Wayland requests terminated with a
  // commit. This must be isolated from anything else that may issue a commit
  // otherwise a half-completed batch may be committed, resulting in a protocol
  // error and the present operation failing
  VkResult result;
  INTERLOCKED_SECTION(wlWm.surfaceLock,
  {
    result = vkQueuePresentKHR(queue, presentInfo);
  });

  if (result != VK_SUCCESS)
  {
    DEBUG_ERROR("Failed to present swapchain image (VkResult: %d)", result);
    return false;
  }

  return true;
}
#endif
