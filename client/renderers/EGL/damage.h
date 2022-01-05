/**
 * Looking Glass
 * Copyright © 2017-2022 The Looking Glass Authors
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

#include <stdbool.h>
#include "common/KVMFR.h"
#include "interface/renderer.h"
#include "desktop_rects.h"

struct DesktopDamage
{
  int count;
  FrameDamageRect rects[KVMFR_MAX_DAMAGE_RECTS];
};

typedef struct EGL_Damage EGL_Damage;

bool egl_damageInit(EGL_Damage ** damage);
void egl_damageFree(EGL_Damage ** damage);

void egl_damageConfigUI(EGL_Damage * damage);
void egl_damageSetup(EGL_Damage * damage, int width, int height);
void egl_damageResize(EGL_Damage * damage, float translateX, float translateY,
    float scaleX, float scaleY);
bool egl_damageRender(EGL_Damage * damage, LG_RendererRotate rotate,
    const struct DesktopDamage * data);
