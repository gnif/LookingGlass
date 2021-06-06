/**
 * Looking Glass
 * Copyright (C) 2017-2021 The Looking Glass Authors
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

#include "draw.h"
#include <stdlib.h>
#include <math.h>

void egl_draw_torus(EGL_Model * model, unsigned int pts, float x, float y, float inner, float outer)
{
  GLfloat * v   = (GLfloat *)malloc(sizeof(GLfloat) * (pts + 1) * 6);
  GLfloat * dst = v;

  for(unsigned int i = 0; i <= pts; ++i)
  {
    const float angle = (i / (float)pts) * M_PI * 2.0f;
    const float c = cos(angle);
    const float s = sin(angle);
    *dst = x + (inner * c); ++dst;
    *dst = y + (inner * s); ++dst;
    *dst = 0.0f; ++dst;
    *dst = x + (outer * c); ++dst;
    *dst = y + (outer * s); ++dst;
    *dst = 0.0f; ++dst;
  }

  egl_model_add_verticies(model, v, NULL, (pts + 1) * 2);
  free(v);
}

void egl_draw_torus_arc(EGL_Model * model, unsigned int pts, float x, float y, float inner, float outer, float s, float e)
{
  GLfloat * v   = (GLfloat *)malloc(sizeof(GLfloat) * (pts + 1) * 6);
  GLfloat * dst = v;

  for(unsigned int i = 0; i <= pts; ++i)
  {
    const float angle = s + ((i / (float)pts) * e);
    const float c = cos(angle);
    const float s = sin(angle);
    *dst = x + (inner * c); ++dst;
    *dst = y + (inner * s); ++dst;
    *dst = 0.0f; ++dst;
    *dst = x + (outer * c); ++dst;
    *dst = y + (outer * s); ++dst;
    *dst = 0.0f; ++dst;
  }

  egl_model_add_verticies(model, v, NULL, (pts + 1) * 2);
  free(v);
}