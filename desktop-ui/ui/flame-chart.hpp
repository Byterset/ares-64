// Flame chart internals, shared by its source files:
//   flame-chart.cpp        the window: toolbar, view (pan/zoom), axis, CPU/RSP/RDP lanes
//   flame-chart-data.cpp   FlameData::collect — the rings read into one window (no ImGui)
//   flame-chart-cache.cpp  the Cache lane, its warnings and tooltip
// Not part of ui.hpp: nothing outside the flame chart needs any of this.
#pragma once

#include "ui.hpp"
#include <n64/n64.hpp>

#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

namespace ares::ui {

// Master clock: 187.5 ticks per microsecond (matches the CPU profiler / RSP viewer).
inline constexpr f64 ticksPerMicrosecond = 187.5;

// A CPU span placed on the window's timeline. Mirrors Profiler::Span (which only
// ever describes a *completed* call) plus an `ongoing` flag for calls that are
// still on the stack: those are synthesised from Profiler::openFrames and run to
// the right edge, so a long function that hasn't returned yet draws as a bar
// instead of a gap.
struct CpuSpan {
  u64 start = 0, end = 0;
  u32 funcAddr = 0;
  u16 depth = 0;
  bool isException = false;
  bool ongoing = false;
};

// A flattened RSP command span, window-relative (tick 0 = window start).
struct RspSpan {
  u64 start = 0, end = 0;
  u16 overlayId = 0;
  u8  commandId = 0;
  bool overhead = false;
  u8  overheadType = 0;
};

// A flattened RDP "DP flush" block, window-relative. The RDP has no per-command
// wall-clock timing, so each flush is one block anchored at its real submit time;
// its width is synthetic (command count * a nominal per-command tick budget).
struct RdpSpan {
  u64 start = 0, end = 0;
  u32 count = 0;
};

using CacheEvent = ares::Nintendo64::CPU::Profiler::CacheEvent;

// Everything the chart shows for one window, read from the profiler and capture
// rings and made window-relative (tick 0 = window start). Refreshed on its own
// schedule by collect(); the view only reads it. Also the natural input for a
// trace export.
struct FlameData {
  std::vector<CpuSpan> spans;                       //sorted by start
  std::vector<RspSpan> rspSpans;                    //sorted by start
  std::vector<RdpSpan> rdpSpans;                    //sorted by start, clamped to not overlap
  std::vector<std::pair<u64, u64>> haltSpans;       //RSP halt intervals
  std::vector<u64> viMarks;                         //VI swap times
  std::vector<CacheEvent> cacheEvents;              //oldest first; `time` window-relative, `evictTime` absolute
  u64 winStart = 0;        //absolute tick of the window's left edge
  u64 rightEdge = 0;       //absolute tick of its right edge (now, or the VI it is pinned to)
  bool ringFull = false;   //the CPU span ring wrapped inside the window: oldest spans may be gone
  bool cacheRingFull = false;
  u32 maxDepth = 0;        //deepest CPU span

  // Synthetic on-screen width for an RDP flush: the RDP has no per-command timing,
  // so a flush block is sized by its command count.
  static constexpr u64 rdpTicksPerCmd = 64;

  // Collection-time decimation input, published by the view after drawing: the
  // absolute tick at the left edge of the last view and its pixels per tick.
  u64 lastViewAbs = 0;
  f64 lastPxPerTick = 0.0;  //0 = not ready

  // Re-collect if due, returning whether it happened. While the game runs only
  // when a new VI has been presented (and the window is pinned to that VI, so the
  // picture is stable per frame); while paused or stepping on every call, so
  // step progress shows live.
  auto collect(u64 windowTicks, int windowIdx, bool running) -> bool;

private:
  // A wide window can hold hundreds of thousands of CPU spans; only ~1 span per
  // (depth, screen pixel) is visible, so narrow spans landing in an occupied cell
  // of last frame's view are dropped at collection. Wide spans are always kept.
  static constexpr u32 GCols = 4096, GDepth = 96;
  std::vector<u32> seenGen;
  u32 decimGen = 0;
  u64 collectedViWrite = ~0ull;
  int collectedWindowIdx = -1;
  bool collectedRunning = false;
};

// Canvas geometry and input for one frame, shared by all lanes.
struct FlameView {
  ImDrawList* dl = nullptr;
  ImVec2 origin, avail;
  f32 canvasR = 0, canvasB = 0;
  f32 clipTop = 0;      //lanes are clipped below the pinned time axis
  f32 rowH = 0;
  u64 viewStart = 0, viewEnd = 0;  //visible range, window-relative ticks
  f64 pxPerTick = 0;
  bool hovered = false;  //mouse over the canvas
  ImVec2 mouse;
  auto toX(u64 t) const -> f32 { return origin.x + (f32)(((s64)t - (s64)viewStart) * pxPerTick); }
};

// --- Cache lane (flame-chart-cache.cpp) --------------------------------------
// The bar under the cursor: its first transfer (into FlameData::cacheEvents; the
// merged ones follow it contiguously), how many merged into it, and the end
// tick of the last.
struct CacheHover {
  const CacheEvent* first = nullptr;
  u32 count = 0;
  u64 lastEnd = 0;
};
auto cacheLaneHeight(f32 rowH) -> f32;  //label band + bar row + gap
auto drawCacheLane(const FlameView& view, const FlameData& data, f32 lanesTop) -> CacheHover;
//warning triangles reach above the lane: draw them once the lanes' clip rect is popped
auto drawCacheWarnings(const FlameView& view) -> void;
auto drawCacheTooltip(const FlameData& data, const CacheHover& hover) -> void;

// --- shared helpers -----------------------------------------------------------

// Stable per-function color from its address: a hash picks one of a curated
// set of muted colours of similar lightness (tracing-viewer style), so bars
// stay distinguishable without turning into confetti and the dark labels are
// readable on every one of them.
inline auto spanColor(u32 addr, bool isException) -> ImU32 {
  if(isException) return IM_COL32(120, 120, 130, 255);  //handlers: muted gray-blue
  static const ImU32 palette[] = {
    IM_COL32(127, 179, 230, 255),  //light blue
    IM_COL32(242, 166,  90, 255),  //soft orange
    IM_COL32(143, 209, 158, 255),  //mint
    IM_COL32(229, 143, 182, 255),  //pink
    IM_COL32(201, 167, 232, 255),  //lavender
    IM_COL32(245, 215, 110, 255),  //soft yellow
    IM_COL32(126, 211, 208, 255),  //teal
    IM_COL32(240, 140, 127, 255),  //salmon
    IM_COL32(168, 198, 134, 255),  //olive
    IM_COL32(224, 176, 140, 255),  //tan
    IM_COL32(157, 180, 224, 255),  //periwinkle
    IM_COL32(217, 165, 214, 255),  //mauve
    IM_COL32(140, 199, 240, 255),  //sky
    IM_COL32(235, 196, 122, 255),  //gold
    IM_COL32(162, 210, 160, 255),  //sage
    IM_COL32(242, 181, 160, 255),  //peach
  };
  u32 h = (addr >> 2) * 2654435761u;  //Knuth multiplicative hash of the word address
  return palette[(h >> 24) % (sizeof(palette) / sizeof(palette[0]))];
}

// Lane name at the left edge, on a dark plate so it stays readable when a bar
// runs underneath it.
inline auto laneLabel(ImDrawList* dl, ImVec2 pos, ImU32 color, const char* text) -> void {
  ImVec2 ts = ImGui::CalcTextSize(text);
  dl->AddRectFilled(ImVec2(pos.x - 2.0_px, pos.y - 1.0_px), ImVec2(pos.x + ts.x + 2.0_px, pos.y + ts.y + 1.0_px),
                    IM_COL32(20, 20, 24, 235), 2.0_px);
  dl->AddText(pos, color, text);
}

inline auto fmtTime(f64 ticks, char* buf, size_t n) -> void {
  f64 us = ticks / ticksPerMicrosecond;
  if(us < 1000.0) snprintf(buf, n, "%.2f us", us);
  else            snprintf(buf, n, "%.3f ms", us / 1000.0);
}

// Like fmtTime, but also reports the raw cycle count
inline auto fmtTimeCyc(f64 ticks, char* buf, size_t n) -> void {
  f64 us = ticks / ticksPerMicrosecond;
  long long cyc = (long long)(ticks + 0.5);
  if(us < 1000.0) snprintf(buf, n, "%.2f us (%lld cyc)", us, cyc);
  else            snprintf(buf, n, "%.3f ms (%lld cyc)", us / 1000.0, cyc);
}

// Signed elapsed time + cycle count (for the measurement marker -> cursor readout).
inline auto fmtDelta(f64 ticks, char* buf, size_t n) -> void {
  char sign = ticks < 0 ? '-' : '+';
  f64 us = std::abs(ticks) / ticksPerMicrosecond;
  long long cyc = (long long)(std::abs(ticks) + 0.5);
  if(us < 1000.0) snprintf(buf, n, "%c%.2f us (%lld cyc)", sign, us, cyc);
  else            snprintf(buf, n, "%c%.3f ms (%lld cyc)", sign, us / 1000.0, cyc);
}

}  // namespace ares::ui
