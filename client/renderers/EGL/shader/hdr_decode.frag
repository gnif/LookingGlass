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

#version 300 es
#extension GL_OES_EGL_image_external_essl3 : enable

precision highp float;

#include "hdr.h"

in  vec2 fragCoord;
out vec4 fragColor;

uniform sampler2D sampler1;

void main()
{
  vec4 pq = texture(sampler1, fragCoord);
  fragColor = vec4(bt2020to709(pq2lin(pq.rgb, 1.0)) * 125.0, pq.a);
}
