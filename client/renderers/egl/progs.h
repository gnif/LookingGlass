/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#ifndef _EGL_PROGS_H
#define _EGL_PROGS_H

static const char egl_vertex_shader_desktop[] = "\
#version 300 es\n\
\
layout(location = 0) in vec3 vertexPosition_modelspace;\
layout(location = 1) in vec2 vertexUV;\
\
uniform vec4 position;\
\
out highp vec2 uv;\
\
void main()\
{\
  gl_Position.xyz = vertexPosition_modelspace; \
  gl_Position.w   = 1.0; \
  gl_Position.x  -= position.x; \
  gl_Position.y  -= position.y; \
  gl_Position.x  *= position.z; \
  gl_Position.y  *= position.w; \
\
  uv = vertexUV;\
}\
";

static const char egl_vertex_shader_mouse[] = "\
#version 300 es\n\
\
layout(location = 0) in vec3 vertexPosition_modelspace;\
layout(location = 1) in vec2 vertexUV;\
\
uniform vec4 mouse;\
\
out highp vec2 uv;\
\
void main()\
{\
  gl_Position.xyz = vertexPosition_modelspace;\
  gl_Position.w   = 1.0;\
  \
  gl_Position.x += 1.0f;\
  gl_Position.y -= 1.0f;\
  \
  gl_Position.x *= mouse.z;\
  gl_Position.y *= mouse.w;\
  \
  gl_Position.x += mouse.x;\
  gl_Position.y -= mouse.y;\
  \
  uv = vertexUV;\
}\
";

static const char egl_fragment_shader_mouse_mono[] = "\
#version 300 es\n\
\
in  highp vec2 uv;\
out highp vec4 color;\
\
uniform sampler2D sampler1;\
 \
void main()\
{\
  highp vec4 tmp = texture(sampler1, uv);\
  if (tmp.rgb == vec3(0.0, 0.0, 0.0))\
    discard;\
  color = tmp;\
}\
";

static const char egl_fragment_shader_rgba[] = "\
#version 300 es\n\
\
in  highp vec2 uv;\
out highp vec4 color;\
\
uniform sampler2D sampler1;\
 \
void main()\
{\
  color = texture(sampler1, uv);\
}\
";

static const char egl_fragment_shader_bgra[] = "\
#version 300 es\n\
\
in  highp vec2 uv;\
out highp vec4 color;\
\
uniform sampler2D sampler1;\
\
void main()\
{\
  color = texture(sampler1, uv);\
}\
";

static const char egl_fragment_shader_yuv[] = "\
#version 300 es\n\
\
in  highp vec2 uv;\
out highp vec4 color;\
\
uniform sampler2D sampler1;\
uniform sampler2D sampler2;\
uniform sampler2D sampler3;\
\
void main()\
{\
  highp vec4 yuv = vec4(\
    texture(sampler1, uv).r,\
    texture(sampler2, uv).r,\
    texture(sampler3, uv).r,\
    1.0\
  );\
  \
  highp mat4 yuv_to_rgb = mat4(\
    1.0,  0.0  ,  1.402, -0.701,\
    1.0, -0.344, -0.714,  0.529,\
    1.0,  1.772,  0.0  , -0.886,\
    1.0,  1.0  ,  1.0  ,  1.0\
  );\
  \
  color = yuv * yuv_to_rgb;\
}\
";

static const char egl_vertex_shader_fps[] = "\
#version 300 es\n\
\
layout(location = 0) in vec3 vertexPosition_modelspace;\
layout(location = 1) in vec2 vertexUV;\
\
uniform vec2 screen;\
uniform vec2 size;\
\
out highp vec2 uv;\
\
void main()\
{\
  highp vec2 pix  = (vec2(1.0, 1.0) / screen); \
  gl_Position.xyz = vertexPosition_modelspace; \
  gl_Position.w   = 1.0; \
  gl_Position.x  *= pix.x * size.x; \
  gl_Position.y  *= pix.y * size.y; \
  gl_Position.x  -= 1.0 - (pix.x * size.x);\
  gl_Position.y  += 1.0 - (pix.y * size.y);\
  gl_Position.x  += pix.x * 10.0; \
  gl_Position.y  -= pix.y * 10.0; \
\
  uv = vertexUV;\
}\
";

static const char egl_fragment_shader_fps[] = "\
#version 300 es\n\
\
in  highp vec2 uv;\
out highp vec4 color;\
\
uniform sampler2D sampler1;\
\
void main()\
{\
  highp vec4 tmp = texture(sampler1, uv);\
  color.r = tmp.b; \
  color.g = tmp.g; \
  color.b = tmp.r; \
  color.a = tmp.a; \
  if (color.a == 0.0) \
  {\
    color.a = 0.5; \
    color.r = 0.0; \
    color.g = 0.0; \
  }\
}\
";


#endif