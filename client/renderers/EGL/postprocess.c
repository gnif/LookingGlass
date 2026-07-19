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
#include "postprocess.h"
#include "filters.h"
#include "app.h"
#include "cimgui.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/stat.h>

#include "common/debug.h"
#include "common/array.h"
#include "common/option.h"
#include "common/paths.h"
#include "common/stringlist.h"
#include "common/stringutils.h"
#include "common/vector.h"

static const EGL_FilterOps * EGL_Filters[] =
{
  &egl_filterDownscaleOps,
  &egl_filterFFXFSR1Ops,
  &egl_filterFFXCASOps,
  &egl_filterGLSLOps
};

struct EGL_PostProcess
{
  Vector filters;
  Vector internalFilters;
  Vector activeFilters;
  Vector effectFilters;
  EGL_Filter  * hdrDecode;
  EGL_Texture * output;
  unsigned int outputX, outputY;
  _Atomic(bool) modified;

  struct
  {
    bool valid;
    EGL_PixelFormat pixFmt;
    unsigned int inputX, inputY;
    int desktopWidth, desktopHeight;
    unsigned int targetX, targetY;
    bool useDMA;
    bool hdrPQ;
    unsigned int outputX, outputY;
    bool outputHDRPQ;
    bool fullFrame;
  }
  config;

  EGL_DesktopRects * rects;
  GLfloat matrix[6];

  StringList presets;
  char * presetDir;
  int activePreset;
  char presetEdit[128];
  char * presetError;
};

void egl_postProcessEarlyInit(void)
{
  static struct Option options[] =
  {
    {
      .module         = "eglFilter",
      .name           = "order",
      .description    = "The order of filters to use",
      .preset         = true,
      .type           = OPTION_TYPE_STRING,
      .value.x_string = ""
    },
    {
      .module         = "egl",
      .name           = "preset",
      .description    = "The initial filter preset to load",
      .type           = OPTION_TYPE_STRING
    },
    { 0 }
  };
  option_register(options);

  for (int i = 0; i < ARRAY_LENGTH(EGL_Filters); ++i)
    egl_filterEarlyInit(EGL_Filters[i]);
}

static void loadPreset(struct EGL_PostProcess * this, const char * name);

static void loadPresetList(struct EGL_PostProcess * this)
{
  DIR * dir = NULL;

  alloc_sprintf(&this->presetDir, "%s/presets", lgConfigDir());
  if (!this->presetDir)
  {
    DEBUG_ERROR("Failed to allocate memory for presets");
    return;
  }

  if (mkdir(this->presetDir, S_IRWXU) < 0 && errno != EEXIST)
  {
    DEBUG_ERROR("Failed to create presets directory: %s", this->presetDir);
    goto fail;
  }

  dir = opendir(this->presetDir);
  if (!dir)
  {
    DEBUG_ERROR("Failed to open presets directory: %s", this->presetDir);
    goto fail;
  }

  this->presets = stringlist_new(true);
  if (!this->presets)
  {
    DEBUG_ERROR("Failed to allocate memory for preset list");
    goto fail;
  }

  struct dirent * entry;
  const char * preset = option_get_string("egl", "preset");
  this->activePreset = -1;

  while ((entry = readdir(dir)) != NULL)
  {
    if (entry->d_type != DT_REG)
      continue;

    DEBUG_INFO("Found preset: %s", entry->d_name);
    char * name = strdup(entry->d_name);
    if (!name)
    {
      DEBUG_ERROR("Failed to allocate memory");
      goto fail;
    }
    stringlist_push(this->presets, name);

    if (preset && strcmp(preset, name) == 0)
      this->activePreset = stringlist_count(this->presets) - 1;
  }
  closedir(dir);


  if (preset)
  {
    if (this->activePreset > -1)
      loadPreset(this, preset);
    else
      DEBUG_WARN("egl:preset '%s' does not exist", preset);
  }

  return;

fail:
  free(this->presetDir);
  this->presetDir = NULL;
  if (dir)
    closedir(dir);
  if (this->presets)
    stringlist_free(&this->presets);
}

static void presetError(struct EGL_PostProcess * this, char * message)
{
  free(this->presetError);
  this->presetError = message;
}

static bool savePreset(struct EGL_PostProcess * this, const char * name)
{
  EGL_Filter * filter;
  vector_forEach(filter, &this->filters)
    egl_filterSaveState(filter);

  size_t orderLen = 0;
  vector_forEach(filter, &this->filters)
    orderLen += strlen(filter->ops.id) + 1;

  char order[orderLen];
  char * p = order;
  vector_forEach(filter, &this->filters)
  {
    strcpy(p, filter->ops.id);
    p += strlen(filter->ops.id);
    *p++ = ';';
  }
  if (p > order)
    p[-1] = '\0';
  option_set_string("eglFilter", "order", order);

  char * path;
  alloc_sprintf(&path, "%s/%s", this->presetDir, name);
  if (!path)
  {
    DEBUG_ERROR("Failed to allocate memory");
    return false;
  }

  FILE * file = fopen(path, "w");
  if (!file)
  {
    const char * strError = strerror(errno);
    DEBUG_ERROR("Failed to open preset \"%s\" for writing: %s", name, strError);
    free(path);

    char * error;
    alloc_sprintf(&error, "Failed to save preset: %s\nError: %s", name, strError);
    if (error)
      presetError(this, error);
    return false;
  }
  free(path);

  DEBUG_INFO("Saving preset: %s", name);
  option_dump_preset(file);
  fclose(file);
  return true;
}

static int stringListIndex(StringList list, const char * str)
{
  unsigned int count = stringlist_count(list);
  for (unsigned int i = 0; i < count; ++i)
    if (strcmp(stringlist_at(list, i), str) == 0)
      return i;
  return INT_MAX;
}

static int compareFilterOrder(const void * a_, const void * b_, void * opaque)
{
  const EGL_Filter * a = *(const EGL_Filter **)a_;
  const EGL_Filter * b = *(const EGL_Filter **)b_;
  StringList order = opaque;

  return stringListIndex(order, a->ops.id) - stringListIndex(order, b->ops.id);
}

static void reorderFilters(struct EGL_PostProcess * this)
{
  StringList order = stringlist_new(false);
  if (!order)
  {
    DEBUG_ERROR("Failed to allocate memory");
    return;
  }

  char * orderStr = strdup(option_get_string("eglFilter", "order"));
  if (!orderStr)
  {
    DEBUG_ERROR("Failed to allocate memory");
    stringlist_free(&order);
    return;
  }

  if (!*orderStr)
  {
    stringlist_free(&order);
    free(orderStr);
    return;
  }

  char * p = orderStr;
  while (*p)
  {
    stringlist_push(order, p);
    char * end = strchr(p, ';');
    if (!end)
      break;
    *end = '\0';
    p = end + 1;
  }

  qsort_r(vector_data(&this->filters), vector_size(&this->filters),
    sizeof(EGL_Filter *), compareFilterOrder, order);

  stringlist_free(&order);
  free(orderStr);
}

static void loadPreset(struct EGL_PostProcess * this, const char * name)
{
  char * path;
  alloc_sprintf(&path, "%s/%s", this->presetDir, name);
  if (!path)
  {
    DEBUG_ERROR("Failed to allocate memory");
    return;
  }

  if (!option_load(path))
  {
    DEBUG_ERROR("Failed to load preset: %s", name);
    free(path);

    char * error;
    alloc_sprintf(&error, "Failed to load preset: %s", name);
    if (error)
      presetError(this, error);
    return;
  }
  free(path);

  DEBUG_INFO("Loading preset: %s", name);
  EGL_Filter * filter;
  vector_forEach(filter, &this->filters)
    egl_filterLoadState(filter);
  reorderFilters(this);
}

static void savePresetAs(struct EGL_PostProcess * this)
{
  if (!savePreset(this, this->presetEdit))
    return;

  for (unsigned i = 0; i < stringlist_count(this->presets); ++i)
  {
    DEBUG_INFO("Saw preset: %s", stringlist_at(this->presets, i));
    if (strcmp(stringlist_at(this->presets, i), this->presetEdit) == 0)
    {
      this->activePreset = i;
      return;
    }
  }

  this->activePreset = stringlist_push(this->presets, strdup(this->presetEdit));
}

static void deletePreset(struct EGL_PostProcess * this)
{
  char * path;
  alloc_sprintf(&path, "%s/%s", this->presetDir,
    stringlist_at(this->presets, this->activePreset));
  if (!path)
  {
    DEBUG_ERROR("Failed to allocate memory");
    return;
  }

  unlink(path);
  free(path);
  stringlist_remove(this->presets, this->activePreset);
  if (this->activePreset >= stringlist_count(this->presets))
    this->activePreset = stringlist_count(this->presets) - 1;
}

static bool presetsUI(struct EGL_PostProcess * this)
{
  if (!this->presets)
    return false;

  bool redraw = false;
  const char * active = "<none>";

  if (this->activePreset >= 0)
    active = stringlist_at(this->presets, this->activePreset);

  if (igBeginCombo("Preset name", active, 0))
  {
    for (unsigned i = 0; i < stringlist_count(this->presets); ++i)
    {
      bool selected = i == this->activePreset;
      if (igSelectable_Bool(stringlist_at(this->presets, i), selected, 0,
            (ImVec2) { 0.0f, 0.0f }))
      {
        this->activePreset = i;
        redraw = true;
        loadPreset(this, stringlist_at(this->presets, this->activePreset));
      }
      if (selected)
        igSetItemDefaultFocus();
    }
    igEndCombo();
  }
  if (igIsItemHovered(ImGuiHoveredFlags_None))
    igSetTooltip("Selecting a preset will load it");

  if (igButton("Save preset", (ImVec2) { 0.0f, 0.0f }))
  {
    if (this->activePreset >= 0)
      savePreset(this, stringlist_at(this->presets, this->activePreset));
    else
      presetError(this, strdup("You must select a preset to save."));
  }

  if (igIsItemHovered(ImGuiHoveredFlags_None) && this->activePreset >= 0)
    igSetTooltip("This will overwrite the preset named: %s",
      stringlist_at(this->presets, this->activePreset));

  igSameLine(0.0f, -1.0f);

  if (igButton("Save preset as...", (ImVec2) { 0.0f, 0.0f }))
  {
    this->presetEdit[0] = '\0';
    igOpenPopup_Str("Save preset as...", ImGuiPopupFlags_None);
  }

  igSameLine(0.0f, -1.0f);

  if (igButton("Delete preset", (ImVec2) { 0.0f, 0.0f }) && this->activePreset >= 0)
    deletePreset(this);

  if (igBeginPopupModal("Save preset as...", NULL, ImGuiWindowFlags_AlwaysAutoResize))
  {
    igText("Enter a name for the new preset:");

    if (!igIsAnyItemActive())
      igSetKeyboardFocusHere(0);

    if (igInputText("##name", this->presetEdit, sizeof(this->presetEdit),
        ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL))
    {
      savePresetAs(this);
      igCloseCurrentPopup();
    }

    if (igButton("Save", (ImVec2) { 0.0f, 0.0f }))
    {
      savePresetAs(this);
      igCloseCurrentPopup();
    }

    igSameLine(0.0f, -1.0f);
    if (igButton("Cancel", (ImVec2) { 0.0f, 0.0f }))
      igCloseCurrentPopup();

    igEndPopup();
  }

  if (this->presetError)
    igOpenPopup_Str("Preset error", ImGuiPopupFlags_None);

  if (igBeginPopupModal("Preset error", NULL, ImGuiWindowFlags_AlwaysAutoResize))
  {
    igText("%s", this->presetError);

    if (!igIsAnyItemActive())
      igSetKeyboardFocusHere(0);

    if (igButton("OK", (ImVec2) { 0.0f, 0.0f }))
    {
      free(this->presetError);
      this->presetError = NULL;
      igCloseCurrentPopup();
    }

    igEndPopup();
  }

  return redraw;
}

static void drawDropTarget(void)
{
  igPushStyleColor_Vec4(ImGuiCol_Separator, (ImVec4) { 1.0f, 1.0f, 0.0f, 1.0f });
  igSeparator();
  igPopStyleColor(1);
}

static void configUI(void * opaque, int * id)
{
  struct EGL_PostProcess * this = opaque;

  bool redraw = false;
  redraw |= presetsUI(this);
  igSeparator();

  static size_t mouseIdx = -1;
  static bool   moving   = false;
  static size_t moveIdx  = 0;

  bool   doMove = false;

  ImVec2 window, pos;
  igGetWindowPos(&window);
  igGetMousePos(&pos);

  EGL_Filter ** filters = vector_data(&this->filters);
  size_t count = vector_size(&this->filters);
  for (size_t i = 0; i < count; ++i)
  {
    EGL_Filter * filter = filters[i];

    if (moving && mouseIdx < moveIdx && i == mouseIdx)
      drawDropTarget();

    igPushID_Ptr(filter);
    bool draw = igCollapsingHeader_BoolPtr(filter->ops.name, NULL, 0);
    if (igIsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
      mouseIdx = i;

    bool active = igIsItemActive();
    if (draw)
      redraw |= egl_filterImguiConfig(filter);
    igPopID();

    if (moving)
    {
      if (!igIsMouseDragging(ImGuiMouseButton_Left, -1.0f))
      {
        moving = false;
        doMove = true;
      }
    }
    else
      if (active && igIsMouseDragging(ImGuiMouseButton_Left, -1.0f))
      {
        moveIdx = mouseIdx;
        moving = true;
      }

    if (moving && mouseIdx > moveIdx && i == mouseIdx)
      drawDropTarget();
  }

  if (moving)
  {
    igSetMouseCursor(ImGuiMouseCursor_Hand);
    igSetTooltip(filters[moveIdx]->ops.name);
  }

  if (doMove && mouseIdx != moveIdx)
  {
    EGL_Filter * tmp = filters[moveIdx];
    if (mouseIdx > moveIdx) // moving down
      memmove(
          filters + moveIdx,
          filters + moveIdx + 1,
          (mouseIdx - moveIdx) * sizeof(EGL_Filter *));
    else // moving up
      memmove(
          filters + mouseIdx + 1,
          filters + mouseIdx,
          (moveIdx - mouseIdx) * sizeof(EGL_Filter *));

    filters[mouseIdx] = tmp;
    redraw = true;
  }

  if (redraw)
  {
    egl_postProcessInvalidate(this);
    app_invalidateWindow(true);
  }
}

bool egl_postProcessInit(EGL_PostProcess ** pp)
{
  EGL_PostProcess * this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("Failed to allocate memory");
    return false;
  }
  atomic_init(&this->modified, false);

  if (!vector_create(&this->filters,
        sizeof(EGL_Filter *), ARRAY_LENGTH(EGL_Filters)))
  {
    DEBUG_ERROR("Failed to allocate the filter list");
    goto error_this;
  }

  if (!vector_create(&this->internalFilters,
        sizeof(EGL_Filter *), ARRAY_LENGTH(EGL_Filters)))
  {
    DEBUG_ERROR("Failed to allocate the filter list");
    goto error_filters;
  }

  if (!vector_create(&this->activeFilters,
        sizeof(EGL_Filter *), ARRAY_LENGTH(EGL_Filters) + 2))
  {
    DEBUG_ERROR("Failed to allocate the active filter list");
    goto error_internal;
  }

  if (!vector_create(&this->effectFilters,
        sizeof(EGL_Filter *), ARRAY_LENGTH(EGL_Filters)))
  {
    DEBUG_ERROR("Failed to allocate the effect filter list");
    goto error_active;
  }

  if (!egl_filterInit(&egl_filterHDRDecodeOps, &this->hdrDecode))
  {
    DEBUG_ERROR("Failed to initialize the HDR decode filter");
    goto error_effect;
  }

  if (!egl_desktopRectsInit(&this->rects, 1))
  {
    DEBUG_ERROR("Failed to initialize the desktop rects");
    goto error_hdr;
  }

  loadPresetList(this);
  reorderFilters(this);
  app_overlayConfigRegisterTab("EGL Filters", configUI, this);

  *pp = this;
  return true;

error_hdr:
  egl_filterFree(&this->hdrDecode);

error_effect:
  vector_destroy(&this->effectFilters);

error_active:
  vector_destroy(&this->activeFilters);

error_internal:
  vector_destroy(&this->internalFilters);

error_filters:
  vector_destroy(&this->filters);

error_this:
  free(this);
  return false;
}

void egl_postProcessFree(EGL_PostProcess ** pp)
{
  if (!*pp)
    return;

  EGL_PostProcess * this = *pp;

  egl_filterFree(&this->hdrDecode);
  vector_destroy(&this->effectFilters);
  vector_destroy(&this->activeFilters);

  EGL_Filter ** filter;
  vector_forEachRef(filter, &this->filters)
    egl_filterFree(filter);
  vector_destroy(&this->filters);

  vector_forEachRef(filter, &this->internalFilters)
    egl_filterFree(filter);
  vector_destroy(&this->internalFilters);

  free(this->presetDir);
  if (this->presets)
    stringlist_free(&this->presets);

  egl_desktopRectsFree(&this->rects);
  free(this->presetError);
  free(this);
  *pp = NULL;
}

static bool addFilter(void * opaque, EGL_Filter * filter)
{
  EGL_PostProcess * this = opaque;

  Vector * filters = filter->ops.type == EGL_FILTER_TYPE_INTERNAL ?
    &this->internalFilters : &this->filters;
  if (!vector_push(filters, &filter))
    return false;

  return true;
}

bool egl_postProcessAdd(EGL_PostProcess * this, const EGL_FilterOps * ops)
{
  if (ops->create)
  {
    if (!ops->create(addFilter, this))
      return false;
  }
  else
  {
    EGL_Filter * filter;
    if (!egl_filterInit(ops, &filter))
      return false;

    if (!addFilter(this, filter))
    {
      egl_filterFree(&filter);
      return false;
    }
  }

  if (ops->type != EGL_FILTER_TYPE_INTERNAL)
    reorderFilters(this);

  egl_postProcessInvalidate(this);
  return true;
}

void egl_postProcessInvalidate(EGL_PostProcess * this)
{
  atomic_store_explicit(&this->modified, true, memory_order_release);
}

bool egl_postProcessConfigModified(EGL_PostProcess * this)
{
  return atomic_load_explicit(&this->modified, memory_order_acquire);
}

bool egl_postProcessNeedsFullFrame(EGL_PostProcess * this)
{
  return this->config.valid && this->config.fullFrame;
}

static bool configMatches(EGL_PostProcess * this, EGL_PixelFormat pixFmt,
    unsigned int inputX, unsigned int inputY,
    int desktopWidth, int desktopHeight,
    unsigned int targetX, unsigned int targetY, bool useDMA, bool hdrPQ)
{
  return
    this->config.valid                         &&
    this->config.pixFmt        == pixFmt       &&
    this->config.inputX        == inputX       &&
    this->config.inputY        == inputY       &&
    this->config.desktopWidth  == desktopWidth &&
    this->config.desktopHeight == desktopHeight &&
    this->config.targetX       == targetX      &&
    this->config.targetY       == targetY      &&
    this->config.useDMA        == useDMA       &&
    this->config.hdrPQ         == hdrPQ;
}

static bool configureChain(EGL_PostProcess * this, EGL_PixelFormat pixFmt,
    unsigned int inputX, unsigned int inputY,
    int desktopWidth, int desktopHeight,
    unsigned int targetX, unsigned int targetY, bool useDMA, bool hdrPQ)
{
  this->config.valid = false;
  vector_clear(&this->activeFilters);
  vector_clear(&this->effectFilters);

  unsigned int outputX = inputX;
  unsigned int outputY = inputY;
  EGL_PixelFormat outputFormat = pixFmt;
  bool inputDMA = useDMA;
  bool fullFrame = false;

  EGL_Filter * filter;
  vector_forEach(filter, &this->internalFilters)
  {
    egl_filterSetOutputResHint(filter, targetX, targetY);

    if (!egl_filterSetup(filter, outputFormat, outputX, outputY,
          desktopWidth, desktopHeight, inputDMA) ||
        !egl_filterPrepare(filter))
      continue;

    if (!vector_push(&this->activeFilters, &filter))
    {
      vector_clear(&this->activeFilters);
      return false;
    }

    fullFrame |= filter->ops.fullFrame;
    egl_filterGetOutputRes(filter, &outputX, &outputY, &outputFormat);
    inputDMA = false;
  }

  const unsigned int    baseOutputX      = outputX;
  const unsigned int    baseOutputY      = outputY;
  const EGL_PixelFormat baseOutputFormat = outputFormat;
  const bool            baseInputDMA     = inputDMA;
  const bool            baseFullFrame    = fullFrame;

  // Effects operate in linear scRGB. Probe them against the format produced
  // by the decoder first so an inactive chain does not compile or execute an
  // otherwise unnecessary conversion pass.
  if (hdrPQ)
  {
    outputFormat = EGL_PF_RGBA16F;
    inputDMA     = false;
  }

  vector_forEach(filter, &this->filters)
  {
    egl_filterSetOutputResHint(filter, targetX, targetY);

    if (!egl_filterSetup(filter, outputFormat, outputX, outputY,
          desktopWidth, desktopHeight, inputDMA) ||
        !egl_filterPrepare(filter))
      continue;

    if (!vector_push(&this->effectFilters, &filter))
    {
      vector_clear(&this->activeFilters);
      vector_clear(&this->effectFilters);
      return false;
    }

    fullFrame |= filter->ops.fullFrame;
    egl_filterGetOutputRes(filter, &outputX, &outputY, &outputFormat);
    inputDMA = false;
  }

  bool effectsActive = vector_size(&this->effectFilters) > 0;
  if (effectsActive && hdrPQ)
  {
    if (!egl_filterSetup(this->hdrDecode,
          baseOutputFormat, baseOutputX, baseOutputY,
          desktopWidth, desktopHeight, baseInputDMA) ||
        !egl_filterPrepare(this->hdrDecode))
    {
      DEBUG_ERROR("Failed to prepare HDR input for the EGL effect chain");
      vector_clear(&this->effectFilters);
      effectsActive = false;
      fullFrame     = baseFullFrame;
    }
    else if (!vector_push(&this->activeFilters, &this->hdrDecode))
    {
      vector_clear(&this->activeFilters);
      vector_clear(&this->effectFilters);
      return false;
    }
  }

  vector_forEach(filter, &this->effectFilters)
    if (!vector_push(&this->activeFilters, &filter))
    {
      vector_clear(&this->activeFilters);
      vector_clear(&this->effectFilters);
      return false;
    }

  if (!effectsActive)
  {
    outputX = baseOutputX;
    outputY = baseOutputY;
  }

  egl_desktopRectsMatrix(this->matrix,
      desktopWidth, desktopHeight, 0.0f, 0.0f, 1.0f, 1.0f, LG_ROTATE_0);

  this->config.pixFmt        = pixFmt;
  this->config.inputX        = inputX;
  this->config.inputY        = inputY;
  this->config.desktopWidth  = desktopWidth;
  this->config.desktopHeight = desktopHeight;
  this->config.targetX       = targetX;
  this->config.targetY       = targetY;
  this->config.useDMA        = useDMA;
  this->config.hdrPQ         = hdrPQ;
  this->config.outputX       = outputX;
  this->config.outputY       = outputY;
  this->config.outputHDRPQ   = hdrPQ && !effectsActive;
  this->config.fullFrame     = fullFrame;
  this->config.valid         = true;
  return true;
}

bool egl_postProcessRun(EGL_PostProcess * this, EGL_Texture * tex,
    EGL_DesktopRects * rects, int desktopWidth, int desktopHeight,
    unsigned int targetX, unsigned int targetY, bool useDMA, bool hdrPQ)
{
  if (targetX == 0 && targetY == 0)
    DEBUG_FATAL("targetX || targetY == 0");

  unsigned int inputX, inputY;
  GLuint _unused;
  EGL_PixelFormat pixFmt;
  if (egl_textureGet(tex, &_unused,
        &inputX, &inputY, &pixFmt) != EGL_TEX_STATUS_OK)
    return false;

  const bool modified =
    atomic_load_explicit(&this->modified, memory_order_acquire);
  const bool reconfigure = modified ||
    !configMatches(this, pixFmt, inputX, inputY,
        desktopWidth, desktopHeight, targetX, targetY, useDMA, hdrPQ);

  if (reconfigure)
  {
    if (modified)
      atomic_exchange_explicit(
          &this->modified, false, memory_order_acq_rel);

    if (!configureChain(this, pixFmt, inputX, inputY,
          desktopWidth, desktopHeight, targetX, targetY, useDMA, hdrPQ))
    {
      egl_postProcessInvalidate(this);
      return false;
    }

    rects = this->rects;
    egl_desktopRectsUpdate(rects, NULL, desktopWidth, desktopHeight);
  }

  if (this->config.fullFrame)
  {
    rects = this->rects;
    egl_desktopRectsUpdate(rects, NULL, desktopWidth, desktopHeight);
  }

  EGL_FilterRects filterRects = {
    .rects  = rects,
    .matrix = this->matrix,
    .width  = desktopWidth,
    .height = desktopHeight,
  };

  EGL_Texture * texture = tex;
  EGL_Filter * filter;
  EGL_Filter * lastFilter = NULL;
  vector_forEach(filter, &this->activeFilters)
  {
    texture = egl_filterRun(filter, &filterRects, texture);
    if (!texture)
    {
      DEBUG_ERROR("EGL filter '%s' failed", filter->ops.id);
      this->output = NULL;
      egl_postProcessInvalidate(this);
      return false;
    }

    if (lastFilter)
      egl_filterRelease(lastFilter);

    lastFilter = filter;
  }

  this->output  = texture;
  this->outputX = this->config.outputX;
  this->outputY = this->config.outputY;
  return true;
}

EGL_Texture * egl_postProcessGetOutput(EGL_PostProcess * this,
    unsigned int * outputX, unsigned int * outputY, bool * hdrPQ)
{
  *outputX = this->outputX;
  *outputY = this->outputY;
  *hdrPQ   = this->config.outputHDRPQ;
  return this->output;
}
