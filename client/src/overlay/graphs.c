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

#include "interface/overlay.h"
#include "cimgui.h"

#include "../main.h"

#include "ll.h"
#include "common/debug.h"
#include "overlay_utils.h"

struct GraphState
{
  bool show;
  struct ll * graphs;
};

static struct GraphState gs = {0};

struct OverlayGraph
{
  const char * name;
  RingBuffer   buffer;
  bool         enabled;
  float        min;
  float        max;
};


static void configCallback(void * udata, int * id)
{
  igCheckbox("Show Timing Graphs", &gs.show);
  igSeparator();

  igBeginTable("split", 2, 0, (ImVec2){}, 0);

  GraphHandle graph;
  for (ll_reset(gs.graphs); ll_walk(gs.graphs, (void **)&graph); )
  {
    igTableNextColumn();
    igCheckbox(graph->name, &graph->enabled);
  }

  igEndTable();
}

static void showTimingKeybind(int sc, void * opaque)
{
  gs.show ^= true;
  app_invalidateWindow(false);
}

static bool graphs_init(void ** udata, const void * params)
{
  gs.graphs = ll_new();
  app_overlayConfigRegister("Performance Metrics", configCallback, NULL);
  app_registerKeybind(KEY_T, showTimingKeybind, NULL,
      "Show frame timing information");
  return true;
}

static void graphs_free(void * udata)
{
  struct OverlayGraph * graph;
  while(ll_shift(gs.graphs, (void **)&graph))
    free(graph);
  ll_free(gs.graphs);
}

struct BufferMetrics
{
  float min;
  float max;
  float sum;
  float avg;
  float freq;
};

static bool rbCalcMetrics(int index, void * value_, void * udata_)
{
  float * value = value_;
  struct BufferMetrics * udata = udata_;

  if (index == 0)
  {
    udata->min = *value;
    udata->max = *value;
    udata->sum = *value;
    return true;
  }

  if (udata->min > *value)
    udata->min = *value;

  if (udata->max < *value)
    udata->max = *value;

  udata->sum += *value;
  return true;
}

static int graphs_render(void * udata, bool interactive,
    struct Rect * windowRects, int maxRects)
{
  if (!gs.show)
    return 0;

  float fontSize = igGetFontSize();

  GraphHandle graph;
  int graphCount = 0;
  for (ll_reset(gs.graphs); ll_walk(gs.graphs, (void **)&graph); )
    if (graph->enabled)
      ++graphCount;

  ImVec2 pos = {0.0f, 0.0f};
  igSetNextWindowBgAlpha(0.4f);
  igSetNextWindowPos(pos, ImGuiCond_FirstUseEver, pos);
  igSetNextWindowSize(
      (ImVec2){
        28.0f * fontSize,
        7.0f  * fontSize * graphCount
      },
      ImGuiCond_FirstUseEver);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoNav;
  if (!interactive)
    flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDecoration;

  igBegin("Performance Metrics",  NULL, flags);

  ImVec2 winSize;
  igGetContentRegionAvail(&winSize);
  const float height = (winSize.y / graphCount)
    - igGetStyle()->ItemSpacing.y;

  for (ll_reset(gs.graphs); ll_walk(gs.graphs, (void **)&graph); )
  {
    if (!graph->enabled)
      continue;

    struct BufferMetrics metrics = {};
    ringbuffer_forEach(graph->buffer, rbCalcMetrics, &metrics, false);

    if (metrics.sum > 0.0f)
    {
      metrics.avg  = metrics.sum / ringbuffer_getCount(graph->buffer);
      metrics.freq = 1000.0f / metrics.avg;
    }

    char title[64];
    snprintf(title, sizeof(title),
        "%s: min:%4.2f max:%4.2f avg:%4.2f/%4.2fHz",
        graph->name, metrics.min, metrics.max, metrics.avg, metrics.freq);

    igPlotLinesFloatPtr(
        "",
        (float *)ringbuffer_getValues(graph->buffer),
        ringbuffer_getLength(graph->buffer),
        ringbuffer_getStart (graph->buffer),
        title,
        graph->min,
        graph->max,
        (ImVec2){ winSize.x, height },
        sizeof(float));
  };

  overlayGetImGuiRect(windowRects);
  igEnd();
  return 1;
}

struct LG_OverlayOps LGOverlayGraphs =
{
  .name           = "Graphs",
  .init           = graphs_init,
  .free           = graphs_free,
  .render         = graphs_render
};

GraphHandle overlayGraph_register(const char * name, RingBuffer buffer, float min, float max)
{
  struct OverlayGraph * graph = malloc(sizeof(*graph));
  graph->name    = name;
  graph->buffer  = buffer;
  graph->enabled = true;
  graph->min     = min;
  graph->max     = max;
  ll_push(gs.graphs, graph);
  return graph;
}

void overlayGraph_unregister(GraphHandle handle)
{
  handle->enabled = false;
}

void overlayGraph_iterate(void (*callback)(GraphHandle handle, const char * name,
      bool * enabled, void * udata), void * udata)
{
  GraphHandle graph;
  for (ll_reset(gs.graphs); ll_walk(gs.graphs, (void **)&graph); )
    callback(graph, graph->name, &graph->enabled, udata);
}
