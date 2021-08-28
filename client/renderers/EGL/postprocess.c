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

#include "postprocess.h"
#include "filters.h"
#include "app.h"
#include "cimgui.h"

#include <dirent.h>
#include <errno.h>
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
  &egl_filterFFXCASOps
};

struct EGL_PostProcess
{
  Vector filters;
  GLuint output;
  unsigned int outputX, outputY;
  _Atomic(bool) modified;

  EGL_Model * model;

  StringList presets;
  char * presetDir;
  int activePreset;
  char presetEdit[128];
};

void egl_postProcessEarlyInit(void)
{
  for(int i = 0; i < ARRAY_LENGTH(EGL_Filters); ++i)
    EGL_Filters[i]->earlyInit();
}

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
  }
  closedir(dir);

  this->activePreset = -1;
  return;

fail:  
  free(this->presetDir);
  this->presetDir = NULL;
  if (dir)
    closedir(dir);
  if (this->presets)
    stringlist_free(&this->presets);
}

static void savePreset(struct EGL_PostProcess * this, const char * name)
{
  EGL_Filter * filter;
  vector_forEach(filter, &this->filters)
    egl_filterSaveState(filter);

  char * path;
  alloc_sprintf(&path, "%s/%s", this->presetDir, name);
  if (!path)
  {
    DEBUG_ERROR("Failed to allocate memory");
    return;
  }

  FILE * file = fopen(path, "w");
  if (!file)
  {
    DEBUG_ERROR("Failed to open preset \"%s\" for writing: %s", name, strerror(errno));
    free(path);
    return;
  }
  free(path);

  DEBUG_INFO("Saving preset: %s", name);
  option_dump(file, "eglFilter");
  fclose(file);
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
    return;
  }
  free(path);

  DEBUG_INFO("Loading preset: %s", name);
  EGL_Filter * filter;
  vector_forEach(filter, &this->filters)
    egl_filterLoadState(filter);
}

static void createPreset(struct EGL_PostProcess * this)
{
  DEBUG_INFO("Create preset: %s", this->presetEdit);
  char * name = strdup(this->presetEdit);
  this->activePreset = stringlist_push(this->presets, name);
  savePreset(this, name);
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
      if (igSelectableBool(stringlist_at(this->presets, i), selected, 0, (ImVec2) { 0.0f, 0.0f }))
        this->activePreset = i;
      if (selected)
        igSetItemDefaultFocus();
    }
    igEndCombo();
  }

  if (igButton("Load preset", (ImVec2) { 0.0f, 0.0f }) && this->activePreset >= 0)
  {
    redraw = true;
    loadPreset(this, stringlist_at(this->presets, this->activePreset));
  }

  igSameLine(0.0f, -1.0f);

  if (igButton("Save preset", (ImVec2) { 0.0f, 0.0f }) && this->activePreset >= 0)
    savePreset(this, stringlist_at(this->presets, this->activePreset));

  if (igIsItemHovered(ImGuiHoveredFlags_None) && this->activePreset >= 0)
    igSetTooltip("This will overwrite the preset named: %s",
      stringlist_at(this->presets, this->activePreset));

  igSameLine(0.0f, -1.0f);

  if (igButton("Create preset", (ImVec2) { 0.0f, 0.0f }))
  {
    this->presetEdit[0] = '\0';
    igOpenPopup("Create preset", ImGuiPopupFlags_None);
  }

  if (igBeginPopupModal("Create preset", NULL, ImGuiWindowFlags_AlwaysAutoResize))
  {
    igText("Enter a name for the new preset:");

    if (!igIsAnyItemActive())
      igSetKeyboardFocusHere(0);

    if (igInputText("##name", this->presetEdit, sizeof(this->presetEdit),
        ImGuiInputTextFlags_EnterReturnsTrue, NULL, NULL))
    {
      createPreset(this);
      igCloseCurrentPopup();
    }

    if (igButton("Create", (ImVec2) { 0.0f, 0.0f }))
    {
      createPreset(this);
      igCloseCurrentPopup();
    }

    igSameLine(0.0f, -1.0f);
    if (igButton("Cancel", (ImVec2) { 0.0f, 0.0f }))
      igCloseCurrentPopup();

    igEndPopup();
  }

  return redraw;
}

static void drawDropTarget(void)
{
  igPushStyleColorVec4(ImGuiCol_Separator, (ImVec4) { 1.0f, 1.0f, 0.0f, 1.0f });
  igSeparator();
  igPopStyleColor(1);
}

static void configUI(void * opaque, int * id)
{
  struct EGL_PostProcess * this = opaque;

  bool redraw = false;
  redraw |= presetsUI(this);

  static size_t mouseIdx = -1;
  static bool   moving   = false;
  static size_t moveIdx  = 0;
  bool doMove = false;

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

    igPushIDPtr(filter);
    bool draw = igCollapsingHeaderBoolPtr(filter->ops.name, NULL, 0);
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

  if (doMove)
  {
    EGL_Filter * tmp = filters[moveIdx];
    if (mouseIdx > moveIdx) // moving down
      memmove(filters + moveIdx, filters + moveIdx + 1, (mouseIdx - moveIdx) * sizeof(EGL_Filter *));
    else // moving up
      memmove(filters + mouseIdx + 1, filters + mouseIdx, (moveIdx - mouseIdx) * sizeof(EGL_Filter *));
    filters[mouseIdx] = tmp;
  }

  if (redraw)
  {
    atomic_store(&this->modified, true);
    app_invalidateWindow(false);
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

  if (!vector_create(&this->filters, sizeof(EGL_Filter *), ARRAY_LENGTH(EGL_Filters)))
  {
    DEBUG_ERROR("Failed to allocate the filter list");
    goto error_this;
  }

  if (!egl_modelInit(&this->model))
  {
    DEBUG_ERROR("Failed to initialize the model");
    goto error_filters;
  }
  egl_modelSetDefault(this->model, false);

  loadPresetList(this);
  app_overlayConfigRegisterTab("EGL Filters", configUI, this);

  *pp = this;
  return true;

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

  EGL_Filter ** filter;
  vector_forEachRef(filter, &this->filters)
    egl_filterFree(filter);
  vector_destroy(&this->filters);

  free(this->presetDir);
  if (this->presets)
    stringlist_free(&this->presets);

  egl_modelFree(&this->model);
  free(this);
  *pp = NULL;
}

bool egl_postProcessAdd(EGL_PostProcess * this, const EGL_FilterOps * ops)
{
  EGL_Filter * filter;
  if (!egl_filterInit(ops, &filter))
    return false;

  vector_push(&this->filters, &filter);
  return true;
}

bool egl_postProcessConfigModified(EGL_PostProcess * this)
{
  return atomic_load(&this->modified);
}

bool egl_postProcessRun(EGL_PostProcess * this, EGL_Texture * tex,
    unsigned int targetX, unsigned int targetY)
{
  EGL_Filter * lastFilter = NULL;
  unsigned int sizeX, sizeY;

  GLuint texture;
  if (egl_textureGet(tex, &texture, &sizeX, &sizeY) != EGL_TEX_STATUS_OK)
    return false;

  atomic_store(&this->modified, false);

  EGL_Filter * filter;
  vector_forEach(filter, &this->filters)
  {
    egl_filterSetOutputResHint(filter, targetX, targetY);
    egl_filterSetup(filter, tex->format.pixFmt, sizeX, sizeY);

    if (!egl_filterPrepare(filter))
      continue;

    texture = egl_filterRun(filter, this->model, texture);
    egl_filterGetOutputRes(filter, &sizeX, &sizeY);

    if (lastFilter)
      egl_filterRelease(lastFilter);

    lastFilter = filter;
  }

  this->output  = texture;
  this->outputX = sizeX;
  this->outputY = sizeY;
  return true;
}

GLuint egl_postProcessGetOutput(EGL_PostProcess * this,
    unsigned int * outputX, unsigned int * outputY)
{
  *outputX = this->outputX;
  *outputY = this->outputY;
  return this->output;
}
