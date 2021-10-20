/**
 * Looking Glass
 * Copyright © 2017-2021 The Looking Glass Authors
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

#include "desktop_rects.h"
#include "common/debug.h"
#include "common/KVMFR.h"
#include "common/locking.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <GLES3/gl3.h>

#include "util.h"

struct EGL_DesktopRects
{
  GLuint  buffers[2];
  GLuint  vao;
  int     count;
  int     maxCount;
};

bool egl_desktopRectsInit(EGL_DesktopRects ** rects_, int maxCount)
{
  EGL_DesktopRects * rects = malloc(sizeof(*rects));
  if (!rects)
  {
    DEBUG_ERROR("Failed to malloc EGL_DesktopRects");
    return false;
  }
  *rects_ = rects;
  memset(rects, 0, sizeof(*rects));

  glGenVertexArrays(1, &rects->vao);
  glBindVertexArray(rects->vao);

  glGenBuffers(2, rects->buffers);
  glBindBuffer(GL_ARRAY_BUFFER, rects->buffers[0]);
  glBufferData(GL_ARRAY_BUFFER, maxCount * 8 * sizeof(GLfloat), NULL, GL_STREAM_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  GLushort indices[maxCount * 6];
  for (int i = 0; i < maxCount; ++i)
  {
    indices[6 * i + 0] = 4 * i + 0;
    indices[6 * i + 1] = 4 * i + 1;
    indices[6 * i + 2] = 4 * i + 2;
    indices[6 * i + 3] = 4 * i + 0;
    indices[6 * i + 4] = 4 * i + 2;
    indices[6 * i + 5] = 4 * i + 3;
  }

  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, rects->buffers[1]);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof indices, indices, GL_STATIC_DRAW);

  glBindVertexArray(0);

  rects->count    = 0;
  rects->maxCount = maxCount;
  return true;
}

void egl_desktopRectsFree(EGL_DesktopRects ** rects_)
{
  EGL_DesktopRects * rects = *rects_;
  if (!rects)
    return;

  glDeleteVertexArrays(1, &rects->vao);
  glDeleteBuffers(2, rects->buffers);
  free(rects);
  *rects_ = NULL;
}

inline static void rectToVertices(GLfloat * vertex, const FrameDamageRect * rect)
{
  vertex[0] = rect->x;
  vertex[1] = rect->y;
  vertex[2] = rect->x + rect->width;
  vertex[3] = rect->y;
  vertex[4] = rect->x + rect->width;
  vertex[5] = rect->y + rect->height;
  vertex[6] = rect->x;
  vertex[7] = rect->y + rect->height;
}

void egl_desktopRectsUpdate(EGL_DesktopRects * rects, const struct DamageRects * data,
    int width, int height)
{
  if (data && data->count == 0)
  {
    rects->count = 0;
    return;
  }

  GLfloat vertices[(!data || data->count < 0 ? 1 : data->count) * 8];
  if (!data || data->count < 0)
  {
    FrameDamageRect full = {
      .x = 0, .y = 0, .width = width, .height = height,
    };
    rects->count = 1;
    rectToVertices(vertices, &full);
  }
  else
  {
    rects->count = data->count;
    DEBUG_ASSERT(rects->count <= rects->maxCount);

    for (int i = 0; i < rects->count; ++i)
      rectToVertices(vertices + i * 8, data->rects + i);
  }

  glBindBuffer(GL_ARRAY_BUFFER, rects->buffers[0]);
  glBufferSubData(GL_ARRAY_BUFFER, 0, rects->count * 8 * sizeof(GLfloat), vertices);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void desktopToGLSpace(double matrix[6], int width, int height, double translateX,
    double translateY, double scaleX, double scaleY, LG_RendererRotate rotate)
{
  switch (rotate)
  {
    case LG_ROTATE_0:
      matrix[0] = 2.0 * scaleX / width;
      matrix[1] = 0.0;
      matrix[2] = 0.0;
      matrix[3] = -2.0 * scaleY / height;
      matrix[4] = translateX - scaleX;
      matrix[5] = translateY + scaleY;
      return;

    case LG_ROTATE_90:
      matrix[0] = 0.0;
      matrix[1] = -2.0 * scaleY / width;
      matrix[2] = -2.0 * scaleX / height;
      matrix[3] = 0.0;
      matrix[4] = translateX + scaleX;
      matrix[5] = translateY + scaleY;
      return;

    case LG_ROTATE_180:
      matrix[0] = -2.0 * scaleX / width;
      matrix[1] = 0.0;
      matrix[2] = 0.0;
      matrix[3] = 2.0 * scaleY / height;
      matrix[4] = translateX + scaleX;
      matrix[5] = translateY - scaleY;
      return;

    case LG_ROTATE_270:
      matrix[0] = 0.0;
      matrix[1] = 2.0 * scaleY / width;
      matrix[2] = 2.0 * scaleX / height;
      matrix[3] = 0.0;
      matrix[4] = translateX - scaleX;
      matrix[5] = translateY - scaleY;
  }
}

void egl_desktopRectsMatrix(float matrix[6], int width, int height, float translateX,
    float translateY, float scaleX, float scaleY, LG_RendererRotate rotate)
{
  double temp[6];
  desktopToGLSpace(temp, width, height, translateX, translateY, scaleX, scaleY, rotate);
  for (int i = 0; i < 6; ++i)
    matrix[i] = temp[i];
}

void egl_desktopToScreenMatrix(double matrix[6], int frameWidth, int frameHeight,
    double translateX, double translateY, double scaleX, double scaleY, LG_RendererRotate rotate,
    double windowWidth, double windowHeight)
{
  desktopToGLSpace(matrix, frameWidth, frameHeight, translateX, translateY, scaleX, scaleY, rotate);

  double hw = windowWidth  / 2;
  double hh = windowHeight / 2;
  matrix[0] *= hw;
  matrix[1] *= hh;
  matrix[2] *= hw;
  matrix[3] *= hh;
  matrix[4]  = matrix[4] * hw + hw;
  matrix[5]  = matrix[5] * hh + hh;
}

inline static void matrixMultiply(const double matrix[6], double * nx, double * ny, double x, double y)
{
  *nx = matrix[0] * x + matrix[2] * y + matrix[4];
  *ny = matrix[1] * x + matrix[3] * y + matrix[5];
}

struct Rect egl_desktopToScreen(const double matrix[6], const struct FrameDamageRect * rect)
{
  double x1, y1, x2, y2;
  matrixMultiply(matrix, &x1, &y1, rect->x, rect->y);
  matrixMultiply(matrix, &x2, &y2, rect->x + rect->width, rect->y + rect->height);

  int x3 = min(x1, x2);
  int y3 = min(y1, y2);
  return (struct Rect) {
    .x = x3,
    .y = y3,
    .w = ceil(max(x1, x2)) - x3,
    .h = ceil(max(y1, y2)) - y3,
  };
}

void egl_screenToDesktopMatrix(double matrix[6], int frameWidth, int frameHeight,
    double translateX, double translateY, double scaleX, double scaleY, LG_RendererRotate rotate,
    double windowWidth, double windowHeight)
{
  double inverted[6] = {0};
  egl_desktopToScreenMatrix(inverted, frameWidth, frameHeight, translateX, translateY,
      scaleX, scaleY, rotate, windowWidth, windowHeight);

  double det = inverted[0] * inverted[3] - inverted[1] * inverted[2];
  matrix[0] =  inverted[3] / det;
  matrix[1] = -inverted[1] / det;
  matrix[2] = -inverted[2] / det;
  matrix[3] =  inverted[0] / det;
  matrix[4] = (inverted[2] * inverted[5] - inverted[3] * inverted[4]) / det;
  matrix[5] = (inverted[1] * inverted[4] - inverted[0] * inverted[5]) / det;
}

bool egl_screenToDesktop(struct FrameDamageRect * output, const double matrix[6],
    const struct Rect * rect, int width, int height)
{
  double x1, y1, x2, y2;
  matrixMultiply(matrix, &x1, &y1, rect->x - 1, rect->y - 1);
  matrixMultiply(matrix, &x2, &y2, rect->x + rect->w + 1, rect->y + rect->h + 1);

  int x3 = min(x1, x2);
  int y3 = min(y1, y2);
  int x4 = ceil(max(x1, x2));
  int y4 = ceil(max(y1, y2));

  if (x4 < 0 || y4 < 0 || x3 >= width || y3 >= height)
    return false;

  output->x = max(x3, 0);
  output->y = max(y3, 0);
  output->width  = min(width,  x4) - output->x;
  output->height = min(height, y4) - output->y;
  return true;
}

void egl_desktopRectsRender(EGL_DesktopRects * rects)
{
  if (!rects->count)
    return;

  glBindVertexArray(rects->vao);
  glDrawElements(GL_TRIANGLES, 6 * rects->count, GL_UNSIGNED_SHORT, NULL);
  glBindVertexArray(0);
}
