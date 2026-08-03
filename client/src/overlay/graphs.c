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

#include "interface/overlay.h"
#include "cimgui.h"
#include "cimplot.h"

#include "../main.h"
#include "../overlays.h"

#include "common/array.h"
#include "common/debug.h"
#include "common/stringutils.h"
#include "common/time.h"
#include "overlay_utils.h"

#include <math.h>

struct GraphState
{
  bool            show;
  struct ll     * graphs;
  ImPlotContext * plotContext;
  ImPlotSpec    * plotSpec;
};

static struct GraphState gs = {0};

enum OverlayGraphType
{
  OVERLAY_GRAPH_LINE,
  OVERLAY_GRAPH_FRAME_TIMING,
};

#define TIMING_STATISTIC_COUNT 3

struct OverlayGraph
{
  char                * name;
  RingBuffer            buffer;
  bool                  enabled;
  bool                  compact;
  enum OverlayGraphType type;
  float                 min;
  float                 max;
  float                 yScale[TIMING_STATISTIC_COUNT];
  GraphFormatFn         formatFn;
};


static void configCallback(void * udata, int * id)
{
  igCheckbox("Show timing graphs", &gs.show);
  igSeparator();

  if (igBeginTable("graphs_split", 2, 0, (ImVec2){}, 0))
  {
    GraphHandle graph;
    ll_lock(gs.graphs);
    ll_forEachNL(gs.graphs, item, graph)
    {
      igTableNextColumn();
      igPushID_Ptr(graph);
      igCheckbox(graph->name, &graph->enabled);
      igPopID();
    }
    ll_unlock(gs.graphs);

    igEndTable();
  }
}

static void showTimingKeybind(int sc, void * opaque)
{
  gs.show ^= true;
  app_invalidateWindow(false);
}

static void graphs_earlyInit(void)
{
  gs.graphs = ll_new();
}

static bool graphs_init(void ** udata, const void * params)
{
  gs.plotContext = ImPlot_CreateContext();
  if (!gs.plotContext)
  {
    DEBUG_ERROR("Failed to create ImPlot context");
    return false;
  }

  ImPlot_SetImGuiContext(igGetCurrentContext());
  gs.plotSpec = ImPlotSpec_ImPlotSpec();
  if (!gs.plotSpec)
  {
    DEBUG_ERROR("Failed to create ImPlot specification");
    ImPlot_DestroyContext(gs.plotContext);
    gs.plotContext = NULL;
    return false;
  }

  app_overlayConfigRegister("Performance Metrics", configCallback, NULL);
  app_registerKeybind(KEY_T, showTimingKeybind, NULL,
      "Show frame timing information");
  return true;
}

static void graphs_free(void * udata)
{
  struct OverlayGraph * graph;
  while(ll_shift(gs.graphs, (void **)&graph))
  {
    free(graph->name);
    free(graph);
  }
  ll_free(gs.graphs);
  gs.graphs = NULL;

  ImPlotSpec_destroy(gs.plotSpec);
  gs.plotSpec = NULL;
  ImPlot_DestroyContext(gs.plotContext);
  gs.plotContext = NULL;
}

struct BufferMetrics
{
  float min;
  float max;
  float sum;
  float avg;
  float freq;
  float last;
};

static bool rbCalcMetrics(int index, void * value_, void * udata_)
{
  float * value = value_;
  struct BufferMetrics * udata = udata_;

  if (index == 0)
  {
    udata->min  = *value;
    udata->max  = *value;
    udata->sum  = *value;
    udata->last = *value;
    return true;
  }

  if (udata->min > *value)
    udata->min = *value;

  if (udata->max < *value)
    udata->max = *value;

  udata->sum  += *value;
  udata->last = *value;
  return true;
}

#define TIMING_PLOT_BUCKETS   100
#define TIMING_PLOT_WINDOW_NS 20000000000ULL
#define TIMING_PLOT_BUCKET_NS \
  (TIMING_PLOT_WINDOW_NS / TIMING_PLOT_BUCKETS)
#define TIMING_STAGE_COUNT    6

struct TimingPlotData
{
  uint64_t windowStart;
  uint64_t windowEnd;
  float    stageMin[TIMING_STAGE_COUNT][TIMING_PLOT_BUCKETS];
  float    stageMax[TIMING_STAGE_COUNT][TIMING_PLOT_BUCKETS];
  float    stageSum[TIMING_STAGE_COUNT][TIMING_PLOT_BUCKETS];
  unsigned count[TIMING_PLOT_BUCKETS];
};

static float roundTimingScale(float value)
{
  value = max(value, 1.0f);
  const float magnitude = powf(10.0f, floorf(log10f(value)));
  const float normalized = value / magnitude;
  const float rounded = normalized <= 1.0f ? 1.0f :
    normalized <= 1.5f ? 1.5f :
    normalized <= 2.0f ? 2.0f :
    normalized <= 3.0f ? 3.0f :
    normalized <= 5.0f ? 5.0f :
    normalized <= 7.5f ? 7.5f : 10.0f;
  return rounded * magnitude;
}

static float graphScale(struct OverlayGraph * graph, int scaleId,
    float peak)
{
  float scale = graph->yScale[scaleId];
  if (scale == 0.0f || peak > scale * 0.95f || peak < scale * 0.45f)
    scale = roundTimingScale(peak * 1.2f);

  graph->yScale[scaleId] = scale;
  return scale;
}

static float graphRowWeight(const struct OverlayGraph * graph)
{
  if (graph->type == OVERLAY_GRAPH_FRAME_TIMING)
    return 3.0f;
  return graph->compact ? 0.5f : 1.0f;
}

static bool accumulateTimingSample(int index, void * value_, void * udata_)
{
  struct TimingPlotData    * data   = udata_;
  const OverlayFrameTiming * timing = value_;
  if (timing->timestamp < data->windowStart)
    return true;
  if (timing->timestamp >= data->windowEnd)
    return false;

  const uint64_t offset = timing->timestamp - data->windowStart;
  const int bucket = min(offset / TIMING_PLOT_BUCKET_NS,
      TIMING_PLOT_BUCKETS - 1);
  const float values[TIMING_STAGE_COUNT] = {
    timing->capture,
    timing->postProcess,
    timing->copy,
    timing->ready,
    timing->import,
    timing->render,
  };
  for (int i = 0; i < TIMING_STAGE_COUNT; ++i)
  {
    if (!data->count[bucket])
      data->stageMin[i][bucket] = data->stageMax[i][bucket] = values[i];
    else
    {
      data->stageMin[i][bucket] = min(data->stageMin[i][bucket], values[i]);
      data->stageMax[i][bucket] = max(data->stageMax[i][bucket], values[i]);
    }
    data->stageSum[i][bucket] += values[i];
  }
  ++data->count[bucket];
  return true;
}

static ImPlotFlags graphFlags(bool interactive, bool legend)
{
  ImPlotFlags flags = legend ? ImPlotFlags_None : ImPlotFlags_NoLegend;
  if (!interactive)
    flags |= ImPlotFlags_NoInputs | ImPlotFlags_NoMenus |
      ImPlotFlags_NoBoxSelect;
  return flags;
}

static void renderLineGraph(struct OverlayGraph * graph, bool interactive,
    ImVec2 size)
{
  struct BufferMetrics metrics = {};
  ringbuffer_forEach(graph->buffer, rbCalcMetrics, &metrics, false);

  const int count = ringbuffer_getCount(graph->buffer);
  if (metrics.sum > 0.0f && count > 0)
  {
    metrics.avg  = metrics.sum / count;
    metrics.freq = 1000.0f / metrics.avg;
  }

  const char * description;
  if (graph->formatFn)
    description = graph->formatFn(graph->name,
        metrics.min, metrics.max, metrics.avg, metrics.freq, metrics.last);
  else
  {
    static char text[128];
    snprintf(text, sizeof(text),
        "%s: min %.2f, max %.2f, avg %.2f ms / %.2f Hz",
        graph->name, metrics.min, metrics.max, metrics.avg, metrics.freq);
    description = text;
  }

  char title[256];
  snprintf(title, sizeof(title), "%s###graph_%p", description, (void *)graph);
  if (!ImPlot_BeginPlot(title, size, graphFlags(interactive, false)))
    return;

  ImPlot_SetupAxes(NULL, "ms",
      ImPlotAxisFlags_NoDecorations, ImPlotAxisFlags_None);
  const float valueMax = graphScale(graph, 0,
      count > 0 ? metrics.max : graph->max);
  ImPlot_SetupAxesLimits(0.0, max(1, count - 1), graph->min, valueMax,
      ImPlotCond_Always);

  gs.plotSpec->Offset = ringbuffer_getStart(graph->buffer);
  gs.plotSpec->Stride = sizeof(float);
  gs.plotSpec->Flags  = ImPlotLineFlags_None;
  ImPlot_PlotLine_FloatPtrInt(graph->name,
      ringbuffer_getValues(graph->buffer), count, 1.0, 0.0, *gs.plotSpec);
  ImPlot_EndPlot();
}

static void renderTimingStatistic(struct OverlayGraph * graph,
    const char * statistic, int statisticId,
    const float values[TIMING_STAGE_COUNT][TIMING_PLOT_BUCKETS],
    bool interactive, ImVec2 size)
{
  float cumulative[TIMING_STAGE_COUNT][TIMING_PLOT_BUCKETS] = {};
  float zero[TIMING_PLOT_BUCKETS] = {};
  float xValues[TIMING_PLOT_BUCKETS];
  float peak = 0.0f;
  for (int bucket = 0; bucket < TIMING_PLOT_BUCKETS; ++bucket)
  {
    xValues[bucket] = bucket;
    for (int stage = 0; stage < TIMING_STAGE_COUNT; ++stage)
      cumulative[stage][bucket] = values[stage][bucket] +
        (stage ? cumulative[stage - 1][bucket] : 0.0f);
    peak = max(peak, cumulative[TIMING_STAGE_COUNT - 1][bucket]);
  }
  const float valueMax = graphScale(graph, statisticId, peak);

  char title[160];
  snprintf(title, sizeof(title), "%s %s###graph_%p_%d",
      graph->name, statistic, (void *)graph, statisticId);
  if (!ImPlot_BeginPlot(title, size, graphFlags(interactive, true)))
    return;

  ImPlot_SetupAxes(NULL, "ms",
      ImPlotAxisFlags_NoDecorations,
      ImPlotAxisFlags_None);
  ImPlot_SetupAxesLimits(-0.5, TIMING_PLOT_BUCKETS - 0.5, 0.0, valueMax,
      ImPlotCond_Always);
  ImPlot_SetupLegend(ImPlotLocation_South,
      ImPlotLegendFlags_Outside | ImPlotLegendFlags_Horizontal);

  const char * labels[TIMING_STAGE_COUNT] = {
    "Capture",
    "Post",
    "Copy",
    "Ready",
    "Import",
    "Render",
  };

  gs.plotSpec->Offset = 0;
  gs.plotSpec->Stride = sizeof(float);
  for (int stage = 0; stage < TIMING_STAGE_COUNT; ++stage)
  {
    const ImVec4 color =
      ImPlot_GetColormapColor(stage, ImPlotColormap_Deep);
    gs.plotSpec->FillColor  = color;
    gs.plotSpec->FillAlpha  = 0.35f;
    gs.plotSpec->LineWeight = 1.0f;
    gs.plotSpec->Flags      = ImPlotShadedFlags_None;
    ImPlot_PlotShaded_FloatPtrFloatPtrFloatPtr(labels[stage], xValues,
        stage ? cumulative[stage - 1] : zero, cumulative[stage],
        TIMING_PLOT_BUCKETS, *gs.plotSpec);

    gs.plotSpec->LineColor  = color;
    gs.plotSpec->LineWeight = 1.0f;
    gs.plotSpec->FillAlpha  = 1.0f;
    gs.plotSpec->Flags      = ImPlotLineFlags_None;
    ImPlot_PlotLine_FloatPtrInt(labels[stage], cumulative[stage],
        TIMING_PLOT_BUCKETS, 1.0, 0.0, *gs.plotSpec);
  }

  gs.plotSpec->LineColor  = (ImVec4) {0.0f, 0.0f, 0.0f, -1.0f};
  gs.plotSpec->FillColor  = (ImVec4) {0.0f, 0.0f, 0.0f, -1.0f};
  gs.plotSpec->LineWeight = 1.0f;
  ImPlot_EndPlot();
}

static void renderTimingGraph(struct OverlayGraph * graph, bool interactive,
    ImVec2 size)
{
  struct TimingPlotData data = {};
  data.windowEnd = nanotime() / TIMING_PLOT_BUCKET_NS * TIMING_PLOT_BUCKET_NS;
  data.windowStart = data.windowEnd > TIMING_PLOT_WINDOW_NS ?
    data.windowEnd - TIMING_PLOT_WINDOW_NS : 0;
  ringbuffer_forEach(graph->buffer, accumulateTimingSample, &data, false);

  float minimum[TIMING_STAGE_COUNT][TIMING_PLOT_BUCKETS] = {};
  float maximum[TIMING_STAGE_COUNT][TIMING_PLOT_BUCKETS] = {};
  float average[TIMING_STAGE_COUNT][TIMING_PLOT_BUCKETS] = {};
  for (int bucket = 0; bucket < TIMING_PLOT_BUCKETS; ++bucket)
  {
    if (!data.count[bucket])
      continue;

    for (int stage = 0; stage < TIMING_STAGE_COUNT; ++stage)
    {
      minimum[stage][bucket] = data.stageMin[stage][bucket];
      maximum[stage][bucket] = data.stageMax[stage][bucket];
      average[stage][bucket] =
        data.stageSum[stage][bucket] / data.count[bucket];
    }
  }

  const float spacing = igGetStyle()->ItemSpacing.y;
  const float height  = (size.y - spacing * 2.0f) / 3.0f;
  renderTimingStatistic(graph, "MINIMUM", 0, minimum,
      interactive, (ImVec2) {size.x, height});
  renderTimingStatistic(graph, "MAXIMUM", 1, maximum,
      interactive, (ImVec2) {size.x, height});
  renderTimingStatistic(graph, "AVERAGE", 2, average,
      interactive, (ImVec2) {size.x, height});
}

static int graphs_render(void * udata, bool interactive,
    struct Rect * windowRects, int maxRects)
{
  if (!gs.show)
    return 0;

  float fontSize = igGetFontSize();

  GraphHandle graph;
  int   graphCount = 0;
  float graphRows  = 0.0f;
  ll_lock(gs.graphs);
  ll_forEachNL(gs.graphs, item, graph)
  {
    if (graph->enabled)
    {
      ++graphCount;
      graphRows += graphRowWeight(graph);
    }
  }

  if (!graphCount)
  {
    ll_unlock(gs.graphs);
    return 0;
  }

  ImVec2 pos = {0.0f, 0.0f};
  igSetNextWindowBgAlpha(0.4f);
  igSetNextWindowPos(pos, ImGuiCond_FirstUseEver, pos);
  igSetNextWindowSize(
      (ImVec2){
        28.0f * fontSize,
        7.0f  * fontSize * graphRows
      },
      ImGuiCond_FirstUseEver);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoNav;
  if (!interactive)
    flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDecoration;

  igBegin("Performance Metrics",  NULL, flags);

  ImVec2 winSize = igGetContentRegionAvail();
  const float spacing = igGetStyle()->ItemSpacing.y;
  const float rowHeight =
    (winSize.y - spacing * (graphCount - 1)) / graphRows;

  for (int pass = 0; pass < 2; ++pass)
  {
    ll_forEachNL(gs.graphs, item, graph)
    {
      if (!graph->enabled || graph->compact != (pass == 1))
        continue;

      igPushID_Ptr(graph);
      const float height = rowHeight * graphRowWeight(graph);
      if (graph->type == OVERLAY_GRAPH_FRAME_TIMING)
        renderTimingGraph(graph, interactive, (ImVec2) {winSize.x, height});
      else
        renderLineGraph(graph, interactive, (ImVec2) {winSize.x, height});
      igPopID();
    }
  }
  ll_unlock(gs.graphs);

  overlayGetImGuiRect(windowRects);
  igEnd();
  return 1;
}

struct LG_OverlayOps LGOverlayGraphs =
{
  .name           = "Graphs",
  .earlyInit      = graphs_earlyInit,
  .init           = graphs_init,
  .free           = graphs_free,
  .render         = graphs_render
};

GraphHandle overlayGraph_register(const char * name, RingBuffer buffer,
    float min, float max, GraphFormatFn formatFn)
{
  if (!name || !*name)
  {
    DEBUG_ERROR("graph name must not be empty");
    return NULL;
  }

  struct OverlayGraph * graph = malloc(sizeof(*graph));
  if (!graph)
  {
    DEBUG_ERROR("out of memory");
    return NULL;
  }

  graph->name = lg_strdup(name);
  if (!graph->name)
  {
    DEBUG_ERROR("out of memory");
    free(graph);
    return NULL;
  }

  graph->buffer   = buffer;
  graph->enabled  = true;
  graph->compact  = false;
  graph->type     = OVERLAY_GRAPH_LINE;
  graph->min      = min;
  graph->max      = max;
  for (int i = 0; i < TIMING_STATISTIC_COUNT; ++i)
    graph->yScale[i] = 0.0f;
  graph->formatFn = formatFn;
  ll_push(gs.graphs, graph);
  return graph;
}

GraphHandle overlayGraph_registerFrameTiming(const char * name,
    RingBuffer buffer)
{
  GraphHandle graph = overlayGraph_register(name, buffer, 0.0f, 0.0f, NULL);
  if (graph)
    graph->type = OVERLAY_GRAPH_FRAME_TIMING;
  return graph;
}

void overlayGraph_unregister(GraphHandle handle)
{
  if (!gs.graphs || !handle)
    return;

  ll_removeData(gs.graphs, handle);
  free(handle->name);
  free(handle);

  if (gs.show)
    app_invalidateWindow(false);
}

void overlayGraph_setCompact(GraphHandle handle, bool compact)
{
  if (handle)
    handle->compact = compact;
}

void overlayGraph_iterate(void (*callback)(GraphHandle handle, const char * name,
      bool * enabled, void * udata), void * udata)
{
  GraphHandle graph;
  ll_lock(gs.graphs);
  ll_forEachNL(gs.graphs, item, graph)
    callback(graph, graph->name, &graph->enabled, udata);
  ll_unlock(gs.graphs);
}

void overlayGraph_invalidate(GraphHandle handle)
{
  if (!gs.show)
    return;

  if (handle->enabled)
    app_invalidateWindow(false);
}
