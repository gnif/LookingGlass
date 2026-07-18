/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
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

#include "wayland.h"

#include <stdbool.h>

#include <wayland-client.h>

#include "common/debug.h"

static void cmIntent(void * data, struct wp_color_manager_v1 * cm, uint32_t intent)
{
  if (intent == WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL)
    wlWm.cmHasPerceptualIntent = true;
}

static void cmFeature(void * data, struct wp_color_manager_v1 * cm, uint32_t feature)
{
  switch (feature)
  {
    case WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC:
      wlWm.cmHasParametric = true;
      break;
    case WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES:
      wlWm.cmHasLuminances = true;
      break;
    case WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES:
      wlWm.cmHasMasteringPrimaries = true;
      break;
    default:
      break;
  }
}

static void cmTFNamed(void * data, struct wp_color_manager_v1 * cm, uint32_t tf)
{
  if (tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ)
    wlWm.cmHasTFSt2084PQ = true;
  else if (tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR)
    wlWm.cmHasTFExtLinear = true;
}

static void cmPrimariesNamed(void * data, struct wp_color_manager_v1 * cm,
    uint32_t primaries)
{
  if (primaries == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020)
    wlWm.cmHasPrimariesBT2020 = true;
  else if (primaries == WP_COLOR_MANAGER_V1_PRIMARIES_SRGB)
    wlWm.cmHasPrimariesSRGB = true;
}

static void cmDone(void * data, struct wp_color_manager_v1 * cm)
{
  wlWm.cmFeaturesDone = true;
  wlWm.cmCanDoHDR = wlWm.cmHasParametric &&
    ((wlWm.cmHasTFSt2084PQ && wlWm.cmHasPrimariesBT2020) ||
     (wlWm.cmHasTFExtLinear && wlWm.cmHasPrimariesSRGB));
  DEBUG_INFO("Color management features: parametric:%d luminances:%d "
      "mastering_primaries:%d st2084_pq:%d ext_linear:%d bt2020:%d srgb:%d "
      "can_do_hdr:%d",
      wlWm.cmHasParametric, wlWm.cmHasLuminances,
      wlWm.cmHasMasteringPrimaries,
      wlWm.cmHasTFSt2084PQ, wlWm.cmHasTFExtLinear,
      wlWm.cmHasPrimariesBT2020, wlWm.cmHasPrimariesSRGB,
      wlWm.cmCanDoHDR);
}

static const struct wp_color_manager_v1_listener cmListener = {
  .supported_intent          = cmIntent,
  .supported_feature         = cmFeature,
  .supported_tf_named        = cmTFNamed,
  .supported_primaries_named = cmPrimariesNamed,
  .done                      = cmDone,
};

bool waylandColorMgmtInit(void)
{
  if (!wlWm.colorManager)
    return true;

  wp_color_manager_v1_add_listener(wlWm.colorManager, &cmListener, NULL);
  return true;
}

void waylandColorMgmtFree(void)
{
  if (!wlWm.colorManager)
    return;

  wp_color_manager_v1_destroy(wlWm.colorManager);
  wlWm.colorManager = NULL;
}
