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

#define _GNU_SOURCE
#include "filter.h"
#include "effect.h"
#include "app.h"

#include "common/debug.h"
#include "common/option.h"
#include "common/paths.h"
#include "common/stringlist.h"
#include "common/stringutils.h"
#include "common/time.h"
#include "cimgui.h"
#include "util.h"

#include "basic.vert.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define GLSL_MAX_TEXTURES 16
#define GLSL_EXPR_STACK   128

typedef struct GLSLInput
{
  EGL_Texture * texture;
  bool original;
}
GLSLInput;

typedef struct GLSLPass
{
  char * desc;
  char * hook;
  char * save;
  char * widthExpr;
  char * heightExpr;
  char * whenExpr;
  char * body;

  char ** binds;
  unsigned int bindCount;
  int components;

  EGL_Shader * shader;
  EGL_EffectPass * effectPass;
  EGL_Uniform ** uTextureInfo;
  EGL_Uniform * uInputSize;
  EGL_Uniform * uTargetSize;
  EGL_Uniform * uTime;
  EGL_Uniform * uMousePos;
  EGL_Uniform * uMouseValid;
  EGL_Uniform * uMouseButtons;
  GLSLInput * inputs;
  bool * external;
  bool compiled;
  bool active;
}
GLSLPass;

typedef struct EGL_FilterGLSL
{
  EGL_Filter base;
  struct EGL_FilterGLSL * next;

  char * path;
  char * relative;
  char * error;
  char * runtimeError;
  GLSLPass * passes;
  unsigned int passCount;
  EGL_Effect * effect;

  bool enable;
  bool active;
  EGL_PixelFormat outputFormat;
  unsigned int inputWidth, inputHeight;
  unsigned int targetWidth, targetHeight;
  unsigned int outputWidth, outputHeight;
  EGL_Texture * outputTexture;
  uint64_t timeOrigin;
}
EGL_FilterGLSL;

typedef struct GLSLResource
{
  const char * name;
  EGL_Texture * texture;
  unsigned int width, height;
  bool original;
  bool external;
}
GLSLResource;

typedef struct GLSLContext
{
  const GLSLResource * original;
  const GLSLResource * current;
  const GLSLResource * saved;
  unsigned int savedCount;
  unsigned int targetWidth, targetHeight;
}
GLSLContext;

typedef struct StringBuffer
{
  char * data;
  size_t length;
  size_t capacity;
}
StringBuffer;

static EGL_FilterGLSL * g_filters;
static char * g_shaderPath;

static void glslFree(EGL_Filter * filter);

static bool bufferReserve(StringBuffer * buffer, size_t extra)
{
  if (buffer->length + extra + 1 <= buffer->capacity)
    return true;

  size_t capacity = buffer->capacity ? buffer->capacity : 4096;
  while (capacity < buffer->length + extra + 1)
    capacity *= 2;

  char * data = realloc(buffer->data, capacity);
  if (!data)
    return false;

  buffer->data = data;
  buffer->capacity = capacity;
  return true;
}

static bool bufferAppendN(StringBuffer * buffer, const char * value,
    size_t length)
{
  if (!bufferReserve(buffer, length))
    return false;

  memcpy(buffer->data + buffer->length, value, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
  return true;
}

static bool bufferAppend(StringBuffer * buffer, const char * value)
{
  return bufferAppendN(buffer, value, strlen(value));
}

static bool bufferAppendF(StringBuffer * buffer, const char * format, ...)
{
  va_list ap;
  va_start(ap, format);
  va_list copy;
  va_copy(copy, ap);
  const int length = vsnprintf(NULL, 0, format, copy);
  va_end(copy);

  if (length < 0 || !bufferReserve(buffer, length))
  {
    va_end(ap);
    return false;
  }

  vsnprintf(buffer->data + buffer->length,
      buffer->capacity - buffer->length, format, ap);
  va_end(ap);
  buffer->length += length;
  return true;
}

static char * duplicateRange(const char * start, const char * end)
{
  const size_t length = end - start;
  char * value = malloc(length + 1);
  if (!value)
    return NULL;
  memcpy(value, start, length);
  value[length] = '\0';
  return value;
}

static char * trimDuplicate(const char * start, const char * end)
{
  while (start < end && isspace((unsigned char)*start))
    ++start;
  while (end > start && isspace((unsigned char)end[-1]))
    --end;
  return duplicateRange(start, end);
}

static void setError(char ** error, const char * format, ...)
{
  free(*error);
  *error = NULL;

  va_list ap;
  va_start(ap, format);
  if (valloc_sprintf(error, format, ap) < 0)
    *error = NULL;
  va_end(ap);
}

static bool validIdentifier(const char * value)
{
  if (!value || (!isalpha((unsigned char)*value) && *value != '_'))
    return false;
  for (++value; *value; ++value)
    if (!isalnum((unsigned char)*value) && *value != '_')
      return false;
  return true;
}

static bool enabledInOption(const char * id)
{
  const char * list = option_get_string("eglFilter", "glslEnabled");
  if (!list || !*list)
    return false;

  const size_t idLength = strlen(id);
  while (*list)
  {
    const char * end = strchr(list, ';');
    const size_t length = end ? (size_t)(end - list) : strlen(list);
    if (length == idLength && !memcmp(list, id, length))
      return true;
    if (!end)
      break;
    list = end + 1;
  }
  return false;
}

static void saveEnabledState(void)
{
  size_t length = 1;
  for (EGL_FilterGLSL * filter = g_filters; filter; filter = filter->next)
    if (filter->enable)
      length += strlen(filter->base.ops.id) + 1;

  char * value = malloc(length);
  if (!value)
    return;

  char * write = value;
  for (EGL_FilterGLSL * filter = g_filters; filter; filter = filter->next)
  {
    if (!filter->enable)
      continue;
    if (write != value)
      *write++ = ';';
    const size_t idLength = strlen(filter->base.ops.id);
    memcpy(write, filter->base.ops.id, idLength);
    write += idLength;
  }
  *write = '\0';
  option_set_string("eglFilter", "glslEnabled", value);
  free(value);
}

static void glslEarlyInit(void)
{
  static struct Option options[] =
  {
    {
      .module         = "eglFilter",
      .name           = "glslPath",
      .description    = "Directory containing mpv-style GLSL shader files",
      .type           = OPTION_TYPE_STRING,
      .value.x_string = ""
    },
    {
      .module         = "eglFilter",
      .name           = "glslEnabled",
      .description    = "Enabled runtime GLSL filters",
      .preset         = true,
      .type           = OPTION_TYPE_STRING,
      .value.x_string = ""
    },
    { 0 }
  };
  option_register(options);
}

static const char * nextDescription(const char * cursor, const char * end)
{
  while (cursor < end)
  {
    const char * found = strstr(cursor, "//!DESC ");
    if (!found || found >= end)
      return NULL;
    if (found == cursor || found[-1] == '\n')
      return found;
    cursor = found + 1;
  }
  return NULL;
}

static bool addBind(GLSLPass * pass, const char * start, const char * end)
{
  if (pass->bindCount == GLSL_MAX_TEXTURES)
    return false;

  char * name = trimDuplicate(start, end);
  if (!name || !validIdentifier(name))
  {
    free(name);
    return false;
  }

  char ** binds = realloc(pass->binds,
      (pass->bindCount + 1) * sizeof(*pass->binds));
  if (!binds)
  {
    free(name);
    return false;
  }
  pass->binds = binds;
  pass->binds[pass->bindCount++] = name;
  return true;
}

static bool setDirective(char ** output, const char * start, const char * end)
{
  if (*output)
    return false;
  *output = trimDuplicate(start, end);
  return *output != NULL;
}

static bool parseDirective(EGL_FilterGLSL * filter, GLSLPass * pass,
    const char * start, const char * end)
{
  const char * split = start;
  while (split < end && !isspace((unsigned char)*split))
    ++split;
  const char * value = split;
  while (value < end && isspace((unsigned char)*value))
    ++value;

  const size_t keyLength = split - start;
#define KEY(name) (keyLength == sizeof(name) - 1 && !memcmp(start, name, sizeof(name) - 1))
  if (KEY("HOOK"))
    return setDirective(&pass->hook, value, end);
  if (KEY("BIND"))
    return addBind(pass, value, end);
  if (KEY("SAVE"))
    return setDirective(&pass->save, value, end);
  if (KEY("WIDTH"))
    return setDirective(&pass->widthExpr, value, end);
  if (KEY("HEIGHT"))
    return setDirective(&pass->heightExpr, value, end);
  if (KEY("WHEN"))
    return setDirective(&pass->whenExpr, value, end);
  if (KEY("COMPONENTS"))
  {
    char * text = trimDuplicate(value, end);
    if (!text)
      return false;
    pass->components = atoi(text);
    free(text);
    return pass->components >= 1 && pass->components <= 4;
  }
#undef KEY

  setError(&filter->error, "Unsupported GLSL directive: %.*s",
      (int)keyLength, start);
  return false;
}

static void freePass(GLSLPass * pass)
{
  free(pass->desc);
  free(pass->hook);
  free(pass->save);
  free(pass->widthExpr);
  free(pass->heightExpr);
  free(pass->whenExpr);
  free(pass->body);
  for (unsigned int i = 0; i < pass->bindCount; ++i)
    free(pass->binds[i]);
  free(pass->binds);
  free(pass->uTextureInfo);
  free(pass->inputs);
  free(pass->external);
  egl_shaderFree(&pass->shader);
}

static bool parseFile(EGL_FilterGLSL * filter)
{
  char * source;
  size_t length;
  if (!util_fileGetContents(filter->path, &source, &length))
  {
    setError(&filter->error, "Unable to read shader file");
    return false;
  }

  if (!egl_effectInit(&filter->effect))
    goto fail;

  const char * sourceEnd = source + length;
  const char * marker = nextDescription(source, sourceEnd);
  while (marker)
  {
    const char * blockEnd = nextDescription(marker + 8, sourceEnd);
    if (!blockEnd)
      blockEnd = sourceEnd;

    GLSLPass * passes = realloc(filter->passes,
        (filter->passCount + 1) * sizeof(*filter->passes));
    if (!passes)
      goto fail;
    filter->passes = passes;
    GLSLPass * pass = &filter->passes[filter->passCount++];
    memset(pass, 0, sizeof(*pass));
    pass->components = 4;

    const char * desc = marker + sizeof("//!DESC ") - 1;
    const char * lineEnd = memchr(desc, '\n', blockEnd - desc);
    if (!lineEnd)
      lineEnd = blockEnd;
    pass->desc = trimDuplicate(desc, lineEnd);
    if (!pass->desc)
      goto fail;

    const char * cursor = lineEnd < blockEnd ? lineEnd + 1 : blockEnd;
    const char * body = blockEnd;
    while (cursor < blockEnd)
    {
      lineEnd = memchr(cursor, '\n', blockEnd - cursor);
      if (!lineEnd)
        lineEnd = blockEnd;

      const char * content = cursor;
      while (content < lineEnd && isspace((unsigned char)*content))
        ++content;
      if (content == lineEnd)
      {
        cursor = lineEnd < blockEnd ? lineEnd + 1 : blockEnd;
        continue;
      }

      if (lineEnd - content >= 3 && !memcmp(content, "//!", 3))
      {
        if (!parseDirective(filter, pass, content + 3, lineEnd))
        {
          if (!filter->error)
            setError(&filter->error, "Invalid directive in pass '%s'", pass->desc);
          goto fail;
        }
        cursor = lineEnd < blockEnd ? lineEnd + 1 : blockEnd;
        continue;
      }

      body = cursor;
      break;
    }

    if (!pass->hook ||
        (strcmp(pass->hook, "MAIN") && strcmp(pass->hook, "PREKERNEL")))
    {
      setError(&filter->error, "Pass '%s' does not hook MAIN/PREKERNEL",
          pass->desc);
      goto fail;
    }
    if (!pass->bindCount)
    {
      setError(&filter->error, "Pass '%s' has no bound textures", pass->desc);
      goto fail;
    }

    pass->body = duplicateRange(body, blockEnd);
    pass->inputs = calloc(pass->bindCount, sizeof(*pass->inputs));
    pass->external = calloc(pass->bindCount, sizeof(*pass->external));
    pass->uTextureInfo = calloc(pass->bindCount, sizeof(*pass->uTextureInfo));
    if (!pass->body || !pass->inputs || !pass->external ||
        !pass->uTextureInfo || !egl_shaderInit(&pass->shader) ||
        !egl_effectAddPass(filter->effect, pass->shader, &pass->effectPass))
      goto fail;

    marker = blockEnd < sourceEnd ? blockEnd : NULL;
  }

  free(source);
  if (!filter->passCount)
  {
    setError(&filter->error, "No GLSL hook passes found");
    return false;
  }
  return true;

fail:
  free(source);
  if (!filter->error)
    setError(&filter->error, "Out of memory while parsing shader");
  return false;
}

static const GLSLResource * findResource(const GLSLContext * context,
    const char * name)
{
  if (!strcmp(name, "MAIN") || !strcmp(name, "HOOKED"))
    return context->current;
  if (!strcmp(name, "NATIVE") || !strcmp(name, "NATIVE_CROPPED"))
    return context->original;
  for (unsigned int i = 0; i < context->savedCount; ++i)
    if (!strcmp(context->saved[i].name, name))
      return context->saved + i;
  return NULL;
}

static bool savedBefore(const EGL_FilterGLSL * filter,
    unsigned int passIndex, const char * name)
{
  for (unsigned int i = 0; i < passIndex; ++i)
    if (filter->passes[i].save && !strcmp(filter->passes[i].save, name))
      return true;
  return false;
}

static bool expressionValue(const GLSLContext * context, const char * token,
    double * value)
{
  char * end;
  errno = 0;
  const double number = strtod(token, &end);
  if (!errno && *token && !*end)
  {
    *value = number;
    return true;
  }

  char * name = lg_strdup(token);
  if (!name)
    return false;
  char * component = strrchr(name, '.');
  if (!component)
  {
    free(name);
    return false;
  }
  *component++ = '\0';

  unsigned int width, height;
  if (!strcmp(name, "OUTPUT"))
  {
    width = context->targetWidth;
    height = context->targetHeight;
  }
  else
  {
    const GLSLResource * resource = findResource(context, name);
    if (!resource)
    {
      free(name);
      return false;
    }
    width = resource->width;
    height = resource->height;
  }

  if (!strcmp(component, "w") || !strcmp(component, "width"))
    *value = width;
  else if (!strcmp(component, "h") || !strcmp(component, "height"))
    *value = height;
  else
  {
    free(name);
    return false;
  }
  free(name);
  return true;
}

static bool expressionValueFinite(double value)
{
  _Static_assert(sizeof(double) == sizeof(uint64_t) &&
      DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
      "GLSL expressions require IEEE-754 binary64 doubles");

  /* isfinite() is not valid with -ffinite-math-only under Clang. Inspect the
   * binary64 exponent directly so expression overflow can still be rejected. */
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return (bits & UINT64_C(0x7ff0000000000000)) !=
    UINT64_C(0x7ff0000000000000);
}

static bool evaluateExpression(const GLSLContext * context,
    const char * expression, double fallback, double * result)
{
  if (!expression)
  {
    *result = fallback;
    return true;
  }

  char * copy = lg_strdup(expression);
  if (!copy)
    return false;

  double stack[GLSL_EXPR_STACK];
  unsigned int count = 0;
  char * state;
  for (char * token = strtok_r(copy, " \t\r\n", &state); token;
       token = strtok_r(NULL, " \t\r\n", &state))
  {
    if (!strcmp(token, "!"))
    {
      if (!count)
        goto fail;
      stack[count - 1] = !stack[count - 1];
      continue;
    }

    if (strlen(token) == 1 && strchr("+-*/><=%", *token))
    {
      if (count < 2)
        goto fail;
      const double right = stack[--count];
      double * left = &stack[count - 1];
      switch(*token)
      {
        case '+': *left += right; break;
        case '-': *left -= right; break;
        case '*': *left *= right; break;
        case '/': if (right == 0.0) goto fail; *left /= right; break;
        case '>': *left = *left > right; break;
        case '<': *left = *left < right; break;
        case '=': *left = *left == right; break;
        case '%': if (right == 0.0) goto fail; *left = fmod(*left, right); break;
      }
      continue;
    }

    if (count == GLSL_EXPR_STACK ||
        !expressionValue(context, token, &stack[count++]))
      goto fail;
  }

  if (count != 1 || !expressionValueFinite(stack[0]))
    goto fail;
  *result = stack[0];
  free(copy);
  return true;

fail:
  free(copy);
  return false;
}

static bool hasBind(const GLSLPass * pass, const char * name)
{
  for (unsigned int i = 0; i < pass->bindCount; ++i)
    if (!strcmp(pass->binds[i], name))
      return true;
  return false;
}

static bool appendTextureMacros(StringBuffer * source, const char * name,
    unsigned int index)
{
  return bufferAppendF(source,
      "#define %s_pos (fragCoord)\n"
      "#define %s_size (textureInfo%u.xy)\n"
      "#define %s_pt (textureInfo%u.zw)\n"
      "#define %s_rot mat2(1.0)\n"
      "#define %s_mul 1.0\n"
      "#define %s_raw sampler%u\n"
      "#define %s_tex(pos) texture(sampler%u, (pos))\n"
      "#define %s_texOff(offset) texture(sampler%u, "
        "fragCoord + (offset) * textureInfo%u.zw)\n",
      name, name, index, name, index, name, name, name, index,
      name, index, name, index, index);
}

static bool appendAlias(StringBuffer * source, const char * alias,
    const char * target)
{
  return bufferAppendF(source,
      "#define %s_pos %s_pos\n"
      "#define %s_size %s_size\n"
      "#define %s_pt %s_pt\n"
      "#define %s_rot %s_rot\n"
      "#define %s_mul %s_mul\n"
      "#define %s_raw %s_raw\n"
      "#define %s_tex(pos) %s_tex(pos)\n"
      "#define %s_texOff(offset) %s_texOff(offset)\n",
      alias, target, alias, target, alias, target, alias, target,
      alias, target, alias, target, alias, target, alias, target);
}

static bool compilePass(EGL_FilterGLSL * filter, GLSLPass * pass,
    const bool * external)
{
  StringBuffer source = { 0 };
  if (!bufferAppend(&source, "#version 300 es\n"))
    goto oom;

  bool needsExternal = false;
  for (unsigned int i = 0; i < pass->bindCount; ++i)
    needsExternal |= external[i];
  if (needsExternal && !bufferAppend(&source,
        "#extension GL_OES_EGL_image_external_essl3 : require\n"))
    goto oom;

  if (!bufferAppend(&source,
        "precision highp float;\n"
        "precision highp int;\n"
        "in vec2 fragCoord;\n"
        "out vec4 fragColor;\n"
        "uniform vec2 input_size;\n"
        "uniform vec2 target_size;\n"
        "uniform float time;\n"
        "uniform vec2 mouse_pos;\n"
        "uniform bool mouse_valid;\n"
        "uniform uint mouse_buttons;\n"))
    goto oom;

  for (unsigned int i = 0; i < pass->bindCount; ++i)
    if (!bufferAppendF(&source, "uniform %s sampler%u;\nuniform vec4 textureInfo%u;\n",
          external[i] ? "samplerExternalOES" : "sampler2D", i + 1, i + 1))
      goto oom;

  for (unsigned int i = 0; i < pass->bindCount; ++i)
    if (!appendTextureMacros(&source, pass->binds[i], i + 1))
      goto oom;

  if (hasBind(pass, "HOOKED") && !hasBind(pass, "MAIN") &&
      !appendAlias(&source, "MAIN", "HOOKED"))
    goto oom;
  if (hasBind(pass, "MAIN") && !hasBind(pass, "HOOKED") &&
      !appendAlias(&source, "HOOKED", "MAIN"))
    goto oom;

  if (!bufferAppendN(&source, pass->body, strlen(pass->body)) ||
      !bufferAppend(&source, "\nvoid main() { fragColor = hook(); }\n"))
    goto oom;

  if (!egl_shaderCompile(pass->shader,
        b_shader_basic_vert, b_shader_basic_vert_size,
        source.data, source.length, false, NULL))
  {
    setError(&filter->runtimeError, "Failed to compile pass: %s", pass->desc);
    free(source.data);
    return false;
  }

  egl_shaderAssocTextures(pass->shader, pass->bindCount);
  for (unsigned int i = 0; i < pass->bindCount; ++i)
  {
    char name[32];
    snprintf(name, sizeof(name), "textureInfo%u", i + 1);
    pass->uTextureInfo[i] = egl_shaderGetUniform(pass->shader, name);
    pass->external[i] = external[i];
  }
  pass->uInputSize = egl_shaderGetUniform(pass->shader, "input_size");
  pass->uTargetSize = egl_shaderGetUniform(pass->shader, "target_size");
  pass->uTime = egl_shaderGetUniform(pass->shader, "time");
  pass->uMousePos = egl_shaderGetUniform(pass->shader, "mouse_pos");
  pass->uMouseValid = egl_shaderGetUniform(pass->shader, "mouse_valid");
  pass->uMouseButtons = egl_shaderGetUniform(pass->shader, "mouse_buttons");
  pass->compiled = true;
  free(source.data);
  return true;

oom:
  free(source.data);
  setError(&filter->runtimeError, "Out of memory generating pass: %s", pass->desc);
  return false;
}

static bool updateSaved(GLSLResource * saved, unsigned int * count,
    const GLSLResource * resource)
{
  for (unsigned int i = 0; i < *count; ++i)
    if (!strcmp(saved[i].name, resource->name))
    {
      saved[i] = *resource;
      return true;
    }
  saved[(*count)++] = *resource;
  return true;
}

static void glslLoadState(EGL_Filter * filter)
{
  EGL_FilterGLSL * this = UPCAST(EGL_FilterGLSL, filter);
  this->enable = enabledInOption(this->base.ops.id);
}

static void glslSaveState(EGL_Filter * filter)
{
  saveEnabledState();
}

static bool glslImguiConfig(EGL_Filter * filter)
{
  EGL_FilterGLSL * this = UPCAST(EGL_FilterGLSL, filter);
  bool enable = this->enable;
  igCheckbox("Enabled", &enable);
  igTextWrapped("%s", this->path);
  igText("Passes: %u", this->passCount);

  if (this->error)
    igTextWrapped("Load error: %s", this->error);
  else if (this->runtimeError)
    igTextWrapped("Runtime error: %s", this->runtimeError);
  else if (this->active)
    igText("Resolution: %ux%u -> %ux%u", this->inputWidth,
        this->inputHeight, this->outputWidth, this->outputHeight);
  else
    igText("Inactive");

  if (enable == this->enable)
    return false;
  this->enable = enable;
  return true;
}

static void glslSetOutputResHint(EGL_Filter * filter,
    unsigned int width, unsigned int height)
{
  EGL_FilterGLSL * this = UPCAST(EGL_FilterGLSL, filter);
  this->targetWidth = width;
  this->targetHeight = height;
}

static bool glslSetup(EGL_Filter * filter, EGL_PixelFormat pixFmt,
    unsigned int width, unsigned int height,
    unsigned int desktopWidth, unsigned int desktopHeight, bool useDMA)
{
  EGL_FilterGLSL * this = UPCAST(EGL_FilterGLSL, filter);
  this->active = false;
  this->outputTexture = NULL;
  free(this->runtimeError);
  this->runtimeError = NULL;
  for (unsigned int i = 0; i < this->passCount; ++i)
    this->passes[i].active = false;

  if (!this->enable || this->error)
    return false;

  GLSLResource original = {
    .name = "NATIVE", .width = width, .height = height,
    .original = true, .external = useDMA,
  };
  GLSLResource current = original;
  current.name = "MAIN";
  GLSLResource * saved = calloc(this->passCount, sizeof(*saved));
  if (!saved)
    return false;
  unsigned int savedCount = 0;
  bool mainChanged = false;

  for (unsigned int passIndex = 0; passIndex < this->passCount; ++passIndex)
  {
    GLSLPass * pass = this->passes + passIndex;
    GLSLContext context = {
      .original = &original,
      .current = &current,
      .saved = saved,
      .savedCount = savedCount,
      .targetWidth = this->targetWidth,
      .targetHeight = this->targetHeight,
    };

    bool external[GLSL_MAX_TEXTURES] = { 0 };
    bool missingSaved = false;
    for (unsigned int i = 0; i < pass->bindCount; ++i)
    {
      const GLSLResource * resource = findResource(&context, pass->binds[i]);
      if (!resource)
      {
        if (savedBefore(this, passIndex, pass->binds[i]))
        {
          missingSaved = true;
          break;
        }
        setError(&this->runtimeError, "Pass '%s' binds unknown texture '%s'",
            pass->desc, pass->binds[i]);
        goto fail;
      }
      pass->inputs[i].texture = resource->texture;
      pass->inputs[i].original = resource->original;
      external[i] = resource->external;
    }
    if (missingSaved)
      continue;

    double condition;
    if (!evaluateExpression(&context, pass->whenExpr, 1.0, &condition))
    {
      setError(&this->runtimeError, "Invalid WHEN expression in pass: %s",
          pass->desc);
      goto fail;
    }
    if (condition == 0.0)
      continue;

    double outputWidth, outputHeight;
    if (!evaluateExpression(&context, pass->widthExpr,
          current.width, &outputWidth) ||
        !evaluateExpression(&context, pass->heightExpr,
          current.height, &outputHeight) ||
        outputWidth < 1.0 || outputHeight < 1.0 ||
        outputWidth > UINT_MAX || outputHeight > UINT_MAX)
    {
      setError(&this->runtimeError, "Invalid output size in pass: %s", pass->desc);
      goto fail;
    }

    bool compile = !pass->compiled;
    for (unsigned int i = 0; i < pass->bindCount && !compile; ++i)
      compile = pass->external[i] != external[i];
    if (compile && !compilePass(this, pass, external))
      goto fail;

    /* Uniform handles are resolved by compilePass, so upload texture metadata
     * again after a first compile or sampler-type recompile. */
    for (unsigned int i = 0; i < pass->bindCount; ++i)
    {
      const GLSLResource * resource = findResource(&context, pass->binds[i]);
      egl_uniform4f(pass->uTextureInfo[i], resource->width, resource->height,
          1.0f / resource->width, 1.0f / resource->height);
    }
    egl_uniform2f(pass->uInputSize, width, height);
    egl_uniform2f(pass->uTargetSize, this->targetWidth, this->targetHeight);

    const unsigned int passWidth = lround(outputWidth);
    const unsigned int passHeight = lround(outputHeight);
    if (!egl_effectPassSetup(pass->effectPass, EGL_PF_RGBA16F,
          passWidth, passHeight))
      goto fail;
    pass->active = true;

    const char * save = pass->save ? pass->save : "HOOKED";
    GLSLResource output = {
      .name = save,
      .texture = egl_effectPassGetTexture(pass->effectPass),
      .width = passWidth,
      .height = passHeight,
    };
    if (!strcmp(save, "MAIN") || !strcmp(save, "HOOKED"))
    {
      output.name = "MAIN";
      current = output;
      mainChanged = true;
    }
    else
      updateSaved(saved, &savedCount, &output);
  }

  free(saved);
  if (!mainChanged)
    return false;

  this->active = true;
  this->inputWidth = width;
  this->inputHeight = height;
  this->outputWidth = current.width;
  this->outputHeight = current.height;
  this->outputFormat = EGL_PF_RGBA16F;
  this->outputTexture = current.texture;
  return true;

fail:
  free(saved);
  return false;
}

static void glslGetOutputRes(EGL_Filter * filter, unsigned int * width,
    unsigned int * height, EGL_PixelFormat * pixFmt)
{
  EGL_FilterGLSL * this = UPCAST(EGL_FilterGLSL, filter);
  *width = this->outputWidth;
  *height = this->outputHeight;
  *pixFmt = this->outputFormat;
}

static bool glslPrepare(EGL_Filter * filter)
{
  EGL_FilterGLSL * this = UPCAST(EGL_FilterGLSL, filter);
  return this->active;
}

static EGL_Texture * glslRun(EGL_Filter * filter, EGL_FilterRects * rects,
    EGL_Texture * texture)
{
  EGL_FilterGLSL * this = UPCAST(EGL_FilterGLSL, filter);
  EGL_Texture * inputs[GLSL_MAX_TEXTURES];
  LG_MouseState mouse;
  app_getMouseState(&mouse);
  const float shaderTime =
    (float)((microtime() - this->timeOrigin) * 1e-6);

  for (unsigned int passIndex = 0; passIndex < this->passCount; ++passIndex)
  {
    GLSLPass * pass = this->passes + passIndex;
    if (!pass->active)
      continue;
    for (unsigned int i = 0; i < pass->bindCount; ++i)
      inputs[i] = pass->inputs[i].original ? texture : pass->inputs[i].texture;
    egl_uniform1f(pass->uTime, shaderTime);
    egl_uniform2f(pass->uMousePos, mouse.x, mouse.y);
    egl_uniform1i(pass->uMouseValid, mouse.valid);
    egl_uniform1ui(pass->uMouseButtons, mouse.buttons);
    if (!egl_effectPassRun(pass->effectPass, rects, inputs, pass->bindCount))
      return NULL;
  }
  return this->outputTexture;
}

static EGL_FilterOps glslOps =
{
  .type             = EGL_FILTER_TYPE_EFFECT,
  .fullFrame        = true,
  .free             = glslFree,
  .imguiConfig      = glslImguiConfig,
  .saveState        = glslSaveState,
  .loadState        = glslLoadState,
  .setup            = glslSetup,
  .setOutputResHint = glslSetOutputResHint,
  .getOutputRes     = glslGetOutputRes,
  .prepare          = glslPrepare,
  .run              = glslRun,
};

static bool createFilter(const char * root, const char * relative,
    EGL_Filter ** result)
{
  EGL_FilterGLSL * this = calloc(1, sizeof(*this));
  if (!this)
    return false;

  memcpy(&this->base.ops, &glslOps, sizeof(glslOps));
  if (alloc_sprintf(&this->path, "%s/%s", root, relative) < 0 ||
      alloc_sprintf((char **)&this->base.ops.id, "glsl:%s", relative) < 0)
    goto fail;

  this->relative = lg_strdup(relative);
  this->base.ops.name = lg_strdup(relative);
  this->timeOrigin = microtime();
  if (!this->relative || !this->base.ops.name)
    goto fail;
  char * extension = strrchr((char *)this->base.ops.name, '.');
  if (extension && !strcasecmp(extension, ".glsl"))
    *extension = '\0';

  parseFile(this);
  this->enable = enabledInOption(this->base.ops.id);
  this->next = g_filters;
  g_filters = this;
  *result = &this->base;
  return true;

fail:
  glslFree(&this->base);
  return false;
}

static void glslFree(EGL_Filter * filter)
{
  EGL_FilterGLSL * this = UPCAST(EGL_FilterGLSL, filter);
  EGL_FilterGLSL ** link = &g_filters;
  while (*link && *link != this)
    link = &(*link)->next;
  if (*link)
    *link = this->next;

  for (unsigned int i = 0; i < this->passCount; ++i)
    freePass(this->passes + i);
  egl_effectFree(&this->effect);
  free(this->passes);
  free((char *)this->base.ops.id);
  free((char *)this->base.ops.name);
  free(this->path);
  free(this->relative);
  free(this->error);
  free(this->runtimeError);
  free(this);
}

static bool hasGLSLExtension(const char * name)
{
  const char * extension = strrchr(name, '.');
  return extension && !strcasecmp(extension, ".glsl");
}

static bool scanDirectory(const char * root, const char * relative,
    StringList files)
{
  char * path;
  if (relative && *relative)
    alloc_sprintf(&path, "%s/%s", root, relative);
  else
    path = lg_strdup(root);
  if (!path)
    return false;

  DIR * directory = opendir(path);
  if (!directory)
  {
    DEBUG_ERROR("Unable to open GLSL shader directory '%s': %s",
        path, strerror(errno));
    free(path);
    return false;
  }

  bool result = true;
  struct dirent * entry;
  while ((entry = readdir(directory)))
  {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;

    char * childRelative;
    if (relative && *relative)
      alloc_sprintf(&childRelative, "%s/%s", relative, entry->d_name);
    else
      childRelative = lg_strdup(entry->d_name);
    if (!childRelative)
    {
      result = false;
      break;
    }

    char * childPath;
    alloc_sprintf(&childPath, "%s/%s", root, childRelative);
    if (!childPath)
    {
      free(childRelative);
      result = false;
      break;
    }

    struct stat statbuf;
    if (lstat(childPath, &statbuf) == 0)
    {
      if (S_ISDIR(statbuf.st_mode))
      {
        result = scanDirectory(root, childRelative, files);
        free(childRelative);
      }
      else if (S_ISREG(statbuf.st_mode) && hasGLSLExtension(entry->d_name))
      {
        const unsigned int count = stringlist_count(files);
        stringlist_push(files, childRelative);
        result = stringlist_count(files) == count + 1;
        if (!result)
          free(childRelative);
      }
      else
        free(childRelative);
    }
    else
      free(childRelative);
    free(childPath);

    if (!result)
      break;
  }

  closedir(directory);
  free(path);
  return result;
}

static int comparePaths(const void * left, const void * right)
{
  return strcmp(*(const char * const *)left, *(const char * const *)right);
}

static bool glslCreate(EGL_FilterAddFn add, void * opaque)
{
  const char * configured = option_get_string("eglFilter", "glslPath");
  free(g_shaderPath);
  g_shaderPath = NULL;
  if (configured && *configured)
    g_shaderPath = lg_strdup(configured);
  else
    alloc_sprintf(&g_shaderPath, "%s/shaders", lgConfigDir());
  if (!g_shaderPath)
    return false;
  DEBUG_INFO("Scanning runtime GLSL filters in: %s", g_shaderPath);

  struct stat statbuf;
  if (stat(g_shaderPath, &statbuf) < 0 && (!configured || !*configured))
  {
    if (mkdir(g_shaderPath, S_IRWXU) < 0 && errno != EEXIST)
    {
      DEBUG_ERROR("Unable to create GLSL shader directory '%s': %s",
          g_shaderPath, strerror(errno));
      return false;
    }
  }

  StringList files = stringlist_new(true);
  if (!files)
    return false;
  if (!scanDirectory(g_shaderPath, NULL, files))
  {
    stringlist_free(&files);
    return false;
  }

  const unsigned int count = stringlist_count(files);
  char ** sorted = malloc(count * sizeof(*sorted));
  if (count && !sorted)
  {
    stringlist_free(&files);
    return false;
  }
  for (unsigned int i = 0; i < count; ++i)
    sorted[i] = stringlist_at(files, i);
  if (count > 1)
    qsort(sorted, count, sizeof(*sorted), comparePaths);

  bool result = true;
  for (unsigned int i = 0; i < count; ++i)
  {
    EGL_Filter * filter = NULL;
    if (!createFilter(g_shaderPath, sorted[i], &filter) || !add(opaque, filter))
    {
      if (filter)
        egl_filterFree(&filter);
      result = false;
      break;
    }
    DEBUG_INFO("Loaded runtime GLSL filter: %s", sorted[i]);
  }

  free(sorted);
  stringlist_free(&files);
  return result;
}

EGL_FilterOps egl_filterGLSLOps =
{
  .id        = "glsl",
  .name      = "Runtime GLSL",
  .type      = EGL_FILTER_TYPE_EFFECT,
  .earlyInit = glslEarlyInit,
  .create    = glslCreate,
};
