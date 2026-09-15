// Flame chart window: the toolbar, the view (pan/zoom/scroll, measurement
// marker), the time axis, the CPU call-stack lane and the RSP/RDP lanes with
// their tooltips. Data collection lives in flame-chart-data.cpp, the Cache
// lane in flame-chart-cache.cpp; see flame-chart.hpp.
#include "flame-chart.hpp"

#include "../desktop-ui.hpp"

#include <algorithm>
#include <vector>

namespace ares::ui {

bool showFlameChart = false;

// RSP overlay palette (mirrors the RSP viewer's overlayColor).
static auto rspOverlayColor(u8 overlayId) -> ImU32 {
  static const ImU32 colors[] = {
    IM_COL32(128, 128, 128, 255), IM_COL32(100, 200, 255, 255),
    IM_COL32(100, 255, 150, 255), IM_COL32(255, 255, 100, 255),
    IM_COL32(255, 180, 100, 255), IM_COL32(255, 150, 200, 255),
    IM_COL32(180, 130, 255, 255), IM_COL32(255, 120, 120, 255),
  };
  return colors[overlayId & 7];
}

static auto rspLabel(u8 overheadType, bool overhead, u16 overlayId, u8 commandId) -> string {
  static const char* overheadNames[] = {"?", "Dispatch", "DMA ucode", "DMA cmd.", "Unknown"};
  auto& rcap = ares::Nintendo64::rsp.capture;
  if(overhead) return string{overheadNames[overheadType < 5 ? overheadType : 0]};
  auto& name = rcap.commandNameMap[overlayId & 15][commandId];
  if(name) return name;
  return string{"ovl", hex(overlayId, 1L), ":", hex(commandId, 2L)};
}

auto DrawFlameChart() -> void {
  bool isN64 = emulator && emulator->name == "Nintendo 64";

  // Auto-enable capture + timeline recording while the window is open. The
  // profiler "enabled" flag is shared with the CPU Profiler window, so only
  // tear it down on close when that window is also closed.
  static bool autoActive = false;
  if(!showFlameChart) {
    if(autoActive && isN64) {
      ares::Nintendo64::cpu.profiler.recordTimeline.store(false, std::memory_order_relaxed);
      if(!showCpuProfiler) ares::Nintendo64::cpu.profiler.setEnabled(false);
      // Leave RSP capture running if the RSP viewer is still open (it owns the toggle).
      if(!showRspViewer) ares::Nintendo64::rsp.capture.enabled.store(false, std::memory_order_relaxed);
    }
    autoActive = false;
    return;
  }

  ImGui::SetNextWindowSize(ImVec2(960_px, 620_px), ImGuiCond_FirstUseEver);
  if(!ImGui::Begin("Flame Chart", &showFlameChart)) {
    ImGui::End();
    settings.general.showFlameChart = showFlameChart;
    return;
  }

  if(!isN64) {
    ImGui::TextUnformatted("Flame chart only available for Nintendo 64.");
    ImGui::End();
    settings.general.showFlameChart = true;
    return;
  }

  auto& prof = ares::Nintendo64::cpu.profiler;
  if(!autoActive) {
    prof.setEnabled(true);
    prof.recordTimeline.store(true, std::memory_order_relaxed);
    ares::Nintendo64::rsp.capture.enabled.store(true, std::memory_order_relaxed);
    autoActive = true;
  }

  // Window length selector: integer multiples of one VI period (~16.67 ms at the NTSC 60 Hz field rate). viTicks = 187.5 MHz / 60.
  static constexpr u64 viTicks = 3'125'000;
  static const char* windowItems[] = {
    "16.7 ms", "33.3 ms", "66.7 ms", "133 ms", "267 ms",
  };
  static const u32 windowMul[] = {1, 2, 4, 8, 16};
  static int windowIdx = 1;  //default: 2 VI
  u64 windowTicks = (u64)windowMul[windowIdx] * viTicks;

  // The collected window (see FlameData::collect for the refresh policy).
  // "Running" means nothing holds the emulation: not paused, not in an RSP/RDP
  // step mode.
  static FlameData data;
  bool running = !program.paused
              && !ares::Nintendo64::rsp.capture.stepMode.load(std::memory_order_relaxed)
              && !ares::Nintendo64::rdp.capture.stepMode.load(std::memory_order_relaxed);
  data.collect(windowTicks, windowIdx, running);
  const auto& spans = data.spans;
  const u32 maxDepth = data.maxDepth;
  const u64 winStart = data.winStart;

  u64 frameTicks = windowTicks;  //axis/view length (kept name for the renderer below)

  // --- view (pan/zoom) state, in window-relative ticks -----------------------
  static u64 viewStart = 0;
  static u64 viewSpan = 0;
  static bool userAdjusted = false;  //true once the user pans/zooms
  static int shownWindowIdx = -1;
  auto fit = [&]() { viewStart = 0; viewSpan = std::max<u64>(1, frameTicks); };
  // Fit to the whole window until the user takes control; also refit whenever the
  // window length changes.
  if(viewSpan == 0 || !userAdjusted || shownWindowIdx != windowIdx) { fit(); shownWindowIdx = windowIdx; }

  // --- toolbar ---------------------------------------------------------------
  if(ImGui::Button("Fit")) { fit(); userAdjusted = false; }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(150.0_px);
  ImGui::Combo("Window", &windowIdx, windowItems, IM_ARRAYSIZE(windowItems));
  //Cache lane options in a popup (before the counters, so they never shift):
  //conflict-miss warnings per cache — data conflicts are noisier (DMA
  //invalidates, streaming), so off by default — and the reuse distance below
  //which a conflict gets a warning triangle.
  ImGui::SameLine();
  if(ImGui::Button("Cache...")) ImGui::OpenPopup("##cacheOptions");
  if(ImGui::BeginPopup("##cacheOptions")) {
    ImGui::TextDisabled("Conflict-miss warnings");
    ImGui::Checkbox("ICache", &settings.general.flameWarnICache);
    ImGui::SameLine();
    ImGui::Checkbox("DCache", &settings.general.flameWarnDCache);
    int threshold = (int)settings.general.flameWarnThreshold;
    ImGui::SetNextItemWidth(180.0_px);
    if(ImGui::SliderInt("Triangle below", &threshold, 1, 256, "%d fills", ImGuiSliderFlags_Logarithmic)) {
      settings.general.flameWarnThreshold = (u32)threshold;
    }
    ImGui::TextDisabled("a line refetched within this many fills\nof being evicted gets a warning triangle");
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  ImGui::Text("cpu: %zu (max depth: %u)   rsp: %zu   rdp: %zu   halt: %zu",
              spans.size(), maxDepth + 1, data.rspSpans.size(), data.rdpSpans.size(), data.haltSpans.size());
  if(data.ringFull) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "(span cap hit)");
  }
  if(data.cacheRingFull) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "(cache event cap hit)");
  }
  ImGui::Separator();

  // --- canvas ----------------------------------------------------------------
  const f32 rowH = 18.0_px;
  const f32 axisH = 18.0_px;
  const f32 axisGap = 12.0_px;  //room below the axis for the Cache lane's warning triangles
  ImVec2 origin = ImGui::GetCursorScreenPos();
  ImVec2 avail = ImGui::GetContentRegionAvail();
  if(avail.x < 50) avail.x = 50;
  if(avail.y < 50) avail.y = 50;
  ImGui::InvisibleButton("canvas", avail);
  bool hovered = ImGui::IsItemHovered();
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), true);
  dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y), IM_COL32(20, 20, 24, 255));

  // Vertical scroll for the lanes (deep call stacks + the RSP/RDP lanes below
  // them): Shift+wheel, or vertical drag. The time axis stays pinned at the top.
  static f32 laneScroll = 0.0f;
  const f32 laneGap = 19.0_px;  //divider+label gap above each device lane
  const f32 haltH = 6.0_px;     //thin RSP halt/stopped indicator bar
  f32 contentH = cacheLaneHeight(rowH) + (maxDepth + 1) * rowH
              + (laneGap + rowH + haltH) /*RSP + halt bar*/ + (laneGap + rowH) /*RDP*/;
  f32 visibleH = avail.y - axisH - axisGap;
  f32 maxScroll = std::max(0.0f, contentH - visibleH);

  // Zoom around cursor on wheel (Shift+wheel scrolls vertically); pan on drag.
  if(hovered && io.MouseWheel != 0.0f) {
    if(io.KeyShift) {
      laneScroll -= io.MouseWheel * rowH * 2.0f;
    } else {
      f64 pxPerTick = avail.x / (f64)viewSpan;
      f64 mouseTick = viewStart + (io.MousePos.x - origin.x) / pxPerTick;
      f64 factor = std::pow(1.2, -io.MouseWheel);
      f64 newSpan = std::clamp<f64>(viewSpan * factor, 32.0, frameTicks * 8.0);
      f64 newPxPerTick = avail.x / newSpan;
      f64 ns = mouseTick - (io.MousePos.x - origin.x) / newPxPerTick;
      viewStart = (u64)std::max<f64>(0.0, ns);
      viewSpan = (u64)newSpan;
      userAdjusted = true;
    }
  }
  if(ImGui::IsItemActive()) {
    if(io.MouseDelta.x != 0.0f) {
      f64 pxPerTick = avail.x / (f64)viewSpan;
      f64 ns = (f64)viewStart - io.MouseDelta.x / pxPerTick;
      viewStart = (u64)std::max<f64>(0.0, ns);
      userAdjusted = true;
    }
    if(io.MouseDelta.y != 0.0f) laneScroll -= io.MouseDelta.y;
  }
  laneScroll = std::clamp(laneScroll, 0.0f, maxScroll);

  // Measurement marker: a left click (without a pan drag) drops a marker at the
  // clicked instant; right click clears it. Stored in absolute master-clock ticks
  // so it stays pinned to a real moment as the window slides. The marker -> cursor
  // delta is shown live under the time axis (drawn below).
  static u64 markerAbs = 0;
  static bool hasMarker = false;
  static f32 pressX = 0.0f;
  {
    f64 pxPerTickNow = avail.x / (f64)viewSpan;
    if(ImGui::IsItemActivated()) pressX = io.MousePos.x;
    if(hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Left)
              && std::abs(io.MousePos.x - pressX) < 4.0_px) {
      f64 relTick = (f64)viewStart + (io.MousePos.x - origin.x) / pxPerTickNow;
      markerAbs = winStart + (u64)std::max<f64>(0.0, relTick);
      hasMarker = true;
    }
    if(hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) hasMarker = false;
  }

  f64 pxPerTick = avail.x / (f64)viewSpan;
  u64 viewEnd = viewStart + viewSpan;
  // Publish this frame's view (in absolute ticks) for next frame's collection-time decimation grid.
  data.lastViewAbs = winStart + viewStart;
  data.lastPxPerTick = pxPerTick;
  f32 lanesTop = origin.y + axisH + axisGap - laneScroll;
  f32 cpuTop = lanesTop + cacheLaneHeight(rowH);
  f32 clipTop = origin.y + axisH;  //lanes are clipped below the pinned axis

  FlameView view;
  view.dl = dl;
  view.origin = origin;
  view.avail = avail;
  view.canvasR = origin.x + avail.x;
  view.canvasB = origin.y + avail.y;
  view.clipTop = clipTop;
  view.rowH = rowH;
  view.viewStart = viewStart;
  view.viewEnd = viewEnd;
  view.pxPerTick = pxPerTick;
  view.hovered = hovered;
  view.mouse = io.MousePos;
  const f32 canvasR = view.canvasR;
  const f32 canvasB = view.canvasB;

  // Time axis ticks (~every 120px), labelled in us/ms relative to window start.
  {
    f64 targetPx = 120.0_px;
    f64 stepTicks = targetPx / pxPerTick;
    // round step to a "nice" 1/2/5 * 10^k value
    f64 mag = std::pow(10.0, std::floor(std::log10(std::max(1.0, stepTicks))));
    f64 norm = stepTicks / mag;
    f64 nice = norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10;
    u64 step = (u64)std::max(1.0, nice * mag);
    u64 first = (viewStart / step) * step;
    for(u64 t = first; t <= viewEnd; t += step) {
      f32 x = origin.x + (f32)((t - viewStart) * pxPerTick);
      if(x < origin.x || x > origin.x + avail.x) continue;
      dl->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + avail.y), IM_COL32(60, 60, 70, 120));
      char b[24]; fmtTime((f64)t, b, sizeof(b));  //t is already window-relative
      dl->AddText(ImVec2(x + 3_px, origin.y + 2_px), IM_COL32(170, 170, 180, 255), b);
    }
  }

  // Clip the lanes to below the pinned time axis so vertical scrolling doesn't
  // overdraw the axis labels.
  dl->PushClipRect(ImVec2(origin.x, clipTop), ImVec2(origin.x + avail.x, origin.y + avail.y), true);

  // --- Cache lane: first, directly above the CPU call stack --------------------
  CacheHover chover = drawCacheLane(view, data, lanesTop);

  // --- CPU lane: the call stack ------------------------------------------------
  // Spans, sorted by start: iterate and cull. Break once a span starts past the
  // view (nothing later can overlap); skip those entirely left of it.
  //
  // Pixel coalescing: when zoomed out, thousands of spans fall into the same pixels.
  // rowMaxX tracks the last drawn right edge per depth; a span
  // that would add less than a pixel of new content in its row is skipped. This
  // caps the rect count at ~canvas-width * depth regardless of how many spans the
  // window holds. Hovering individual spans only matters when zoomed in (where spans are wider than a pixel and nothing coalesces).
  static std::vector<f32> rowMaxX;
  rowMaxX.assign((size_t)maxDepth + 2, -1e9f);
  const CpuSpan* hover = nullptr;
  for(auto it = spans.begin(); it != spans.end(); ++it) {
    const CpuSpan& s = *it;
    if(s.start > viewEnd) break;     //sorted by start: nothing further can overlap
    if(s.end < viewStart) continue;  //entirely left of the view
    f32 x0 = origin.x + (f32)(((s64)s.start - (s64)viewStart) * pxPerTick);
    f32 x1 = origin.x + (f32)(((s64)s.end - (s64)viewStart) * pxPerTick);
    f32& lx = rowMaxX[s.depth];
    //ongoing calls are the point of the lane: never let coalescing drop one
    if(!s.ongoing && x1 <= lx + 1.0f) continue;  //<1px of new content in this row
    if(x1 - x0 < 1.0f) x1 = x0 + 1.0f;
    x0 = std::max(x0, origin.x);
    x1 = std::min(x1, canvasR);
    if(x1 <= x0) continue;
    //An ongoing bar reaches the right edge, so folding it into the row's coalescing
    //state would suppress everything drawn after it at this depth — real spans from
    //another thread's stack share these depth lanes. Draw it, but leave lx alone.
    if(!s.ongoing) lx = x1;
    f32 y0 = cpuTop + s.depth * rowH;
    f32 y1 = y0 + rowH - 1.0f;
    if(y0 > canvasB) continue;

    bool isHover = hovered && io.MousePos.x >= x0 && io.MousePos.x < x1
                           && io.MousePos.y >= y0 && io.MousePos.y < y1;
    if(isHover) hover = &s;

    ImU32 col = spanColor(s.funcAddr, s.isException);
    if(isHover) col = IM_COL32(255, 255, 255, 255);
    f32 w = x1 - x0;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col, w > 4.0_px ? 2.0_px : 0.0f);
    if(w > 3.0_px) dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 90), 2.0_px);

    // Unfinished calls have no real right edge: cap them with a bright bar at
    // "now" so they don't read as a call that happened to end at the view edge.
    if(s.ongoing) {
      dl->AddRectFilled(ImVec2(std::max(x0, x1 - 2.0_px), y0), ImVec2(x1, y1),
                        IM_COL32(255, 220, 80, 255));
    }

    if(w > 28.0_px) {
      string name = prof.labelFor(s.funcAddr);
      if(s.ongoing) name.append("...");
      dl->PushClipRect(ImVec2(x0 + 2_px, y0), ImVec2(x1 - 1_px, y1), true);
      dl->AddText(ImVec2(x0 + 3_px, y0 + 2_px), IM_COL32(15, 15, 18, 255), name.data());
      dl->PopClipRect();
    }
  }

  // --- RSP lane: command spans on the same axis, below the CPU call stack -----
  const RspSpan* rhover = nullptr;
  const RdpSpan* dhover = nullptr;
  {
    f32 cpuBottom = cpuTop + (maxDepth + 1) * rowH;
    f32 labelY = cpuBottom + 4.0_px;
    f32 rspTop = labelY + 15.0_px;
    dl->AddLine(ImVec2(origin.x, labelY), ImVec2(origin.x + avail.x, labelY), IM_COL32(70, 70, 80, 200));
    laneLabel(dl, ImVec2(origin.x + 3_px, labelY + 1_px), IM_COL32(150, 200, 255, 255), "RSP");

    f32 rspMaxX = -1e9f;  //per-row pixel coalescing (see CPU lane)
    for(auto it = data.rspSpans.begin(); it != data.rspSpans.end(); ++it) {
      const RspSpan& s = *it;
      if(s.start > viewEnd) break;
      if(s.end < viewStart) continue;
      f32 x0 = origin.x + (f32)(((s64)s.start - (s64)viewStart) * pxPerTick);
      f32 x1 = origin.x + (f32)(((s64)s.end - (s64)viewStart) * pxPerTick);
      if(x1 <= rspMaxX + 1.0f) continue;
      if(x1 - x0 < 1.0f) x1 = x0 + 1.0f;
      x0 = std::max(x0, origin.x);
      x1 = std::min(x1, canvasR);
      if(x1 <= x0) continue;
      rspMaxX = x1;
      f32 y0 = rspTop;
      f32 y1 = y0 + rowH - 1.0f;
      if(y0 > canvasB) continue;

      bool isHover = hovered && io.MousePos.x >= x0 && io.MousePos.x < x1
                             && io.MousePos.y >= y0 && io.MousePos.y < y1;
      if(isHover) rhover = &s;

      ImU32 col = !s.overhead ? rspOverlayColor(s.overlayId)
                : s.overheadType == ares::Nintendo64::RSPCapture::OverheadUnknown ? IM_COL32(80, 60, 95, 255)  // Unknown: muted purple
                :                                                                   IM_COL32(90, 90, 100, 255); // dispatch/DMA: gray
      if(isHover) col = IM_COL32(255, 255, 255, 255);
      f32 w = x1 - x0;
      dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col, w > 4.0_px ? 2.0_px : 0.0f);
      if(w > 3.0_px) dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 90), 2.0_px);
      if(w > 28.0_px) {
        string name = rspLabel(s.overheadType, s.overhead, s.overlayId, s.commandId);
        dl->PushClipRect(ImVec2(x0 + 2_px, y0), ImVec2(x1 - 1_px, y1), true);
        dl->AddText(ImVec2(x0 + 3_px, y0 + 2_px), IM_COL32(15, 15, 18, 255), name.data());
        dl->PopClipRect();
      }
    }

    // RSP hardware-stopped (SP_STATUS.halted / BREAK) bar, directly under the
    // command stream. Only drawn where halted — running shows no bar.
    {
      f32 hy0 = rspTop + rowH;
      f32 hy1 = hy0 + haltH - 1.0f;
      for(auto& hsp : data.haltSpans) {
        if(hsp.first > viewEnd || hsp.second < viewStart) continue;
        f32 x0 = origin.x + (f32)(((s64)hsp.first  - (s64)viewStart) * pxPerTick);
        f32 x1 = origin.x + (f32)(((s64)hsp.second - (s64)viewStart) * pxPerTick);
        if(x1 - x0 < 1.0f) x1 = x0 + 1.0f;
        x0 = std::max(x0, origin.x);
        x1 = std::min(x1, origin.x + avail.x);
        if(x1 <= x0) continue;
        if(hy0 > origin.y + avail.y) continue;
        dl->AddRectFilled(ImVec2(x0, hy0), ImVec2(x1, hy1), IM_COL32(220, 70, 70, 235));
      }
    }

    // --- RDP lane: one block per DP flush, below the RSP lane ------------------
    f32 rdpLabelY = rspTop + rowH + haltH + 4.0_px;
    f32 rdpTop = rdpLabelY + 15.0_px;
    dl->AddLine(ImVec2(origin.x, rdpLabelY), ImVec2(origin.x + avail.x, rdpLabelY), IM_COL32(70, 70, 80, 200));
    laneLabel(dl, ImVec2(origin.x + 3_px, rdpLabelY + 1_px), IM_COL32(150, 255, 200, 255), "RDP");

    for(auto it = data.rdpSpans.begin(); it != data.rdpSpans.end(); ++it) {
      const RdpSpan& s = *it;
      if(s.start > viewEnd) break;
      if(s.end < viewStart) continue;
      f32 x0 = origin.x + (f32)(((s64)s.start - (s64)viewStart) * pxPerTick);
      f32 x1 = origin.x + (f32)(((s64)s.end - (s64)viewStart) * pxPerTick);
      if(x1 - x0 < 1.0f) x1 = x0 + 1.0f;
      x0 = std::max(x0, origin.x);
      x1 = std::min(x1, origin.x + avail.x);
      if(x1 <= x0) continue;
      f32 y0 = rdpTop;
      f32 y1 = y0 + rowH - 1.0f;
      if(y0 > origin.y + avail.y) continue;

      bool isHover = hovered && io.MousePos.x >= x0 && io.MousePos.x < x1
                             && io.MousePos.y >= y0 && io.MousePos.y < y1;
      if(isHover) dhover = &s;

      ImU32 col = isHover ? IM_COL32(255, 255, 255, 255) : IM_COL32(90, 200, 160, 255);
      dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col, 2.0_px);
      dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, 90), 2.0_px);
      if(x1 - x0 > 28.0_px) {
        string name = {"DP ", s.count, " cmds"};
        dl->PushClipRect(ImVec2(x0 + 2_px, y0), ImVec2(x1 - 1_px, y1), true);
        dl->AddText(ImVec2(x0 + 3_px, y0 + 2_px), IM_COL32(15, 15, 18, 255), name.data());
        dl->PopClipRect();
      }
    }
  }

  dl->PopClipRect();  //lanes clip

  drawCacheWarnings(view);

  // VI framebuffer-swap markers: vertical lines over the whole canvas, drawn last
  // (above the lanes) so frame boundaries stay visible.
  for(u64 m : data.viMarks) {
    if(m < viewStart || m > viewEnd) continue;
    f32 x = origin.x + (f32)(((s64)m - (s64)viewStart) * pxPerTick);
    dl->AddLine(ImVec2(x, origin.y + axisH), ImVec2(x, origin.y + avail.y), IM_COL32(255, 90, 90, 150), 1.0_px);
    dl->AddText(ImVec2(x + 2_px, origin.y + axisH + 1_px), IM_COL32(255, 120, 120, 255), "VI");
  }

  // Measurement marker (yellow) + live delta-to-cursor readout under the axis.
  if(hasMarker) {
    f64 markerRel = (f64)markerAbs - (f64)winStart;  //window-relative ticks
    f32 mx = origin.x + (f32)((markerRel - (f64)viewStart) * pxPerTick);
    if(mx >= origin.x && mx <= origin.x + avail.x)
      dl->AddLine(ImVec2(mx, origin.y), ImVec2(mx, origin.y + avail.y), IM_COL32(255, 220, 80, 220), 1.5_px);

    if(hovered) {
      f32 cx = std::clamp(io.MousePos.x, origin.x, origin.x + avail.x);
      dl->AddLine(ImVec2(cx, origin.y + axisH), ImVec2(cx, origin.y + avail.y), IM_COL32(255, 220, 80, 90), 1.0_px);
      //horizontal ruler slightly below the tick labels, marker -> cursor
      f32 ry = origin.y + 16.0_px;
      f32 lx = std::clamp(mx, origin.x, origin.x + avail.x);
      dl->AddLine(ImVec2(lx, ry), ImVec2(cx, ry), IM_COL32(255, 220, 80, 200), 1.0_px);
      f64 cursorAbs = (f64)winStart + (f64)viewStart + (io.MousePos.x - origin.x) / pxPerTick;
      char b[48]; fmtDelta(cursorAbs - (f64)markerAbs, b, sizeof(b));
      ImVec2 ts = ImGui::CalcTextSize(b);
      f32 tx = cx + 4.0_px; if(tx + ts.x > origin.x + avail.x) tx = cx - 4.0_px - ts.x;
      dl->AddText(ImVec2(tx, ry + 2.0_px), IM_COL32(255, 230, 120, 255), b);
    }
  }

  dl->PopClipRect();  //canvas clip

  // RSP hover tooltip.
  if(rhover) {
    string name = rspLabel(rhover->overheadType, rhover->overhead, rhover->overlayId, rhover->commandId);
    char dbuf[48]; fmtTimeCyc((f64)(rhover->end - rhover->start), dbuf, sizeof(dbuf));
    f64 pct = 100.0 * (f64)(rhover->end - rhover->start) / (f64)frameTicks;
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(name.data());
    ImGui::Separator();
    ImGui::Text("RSP  duration: %s  (%.2f%% of window)", dbuf, pct);
    ImGui::EndTooltip();
  }

  // RDP flush tooltip. Width is synthetic (no per-command RDP timing), so report
  // the command count rather than a duration.
  if(dhover) {
    ImGui::BeginTooltip();
    ImGui::Text("DP flush");
    ImGui::Separator();
    ImGui::Text("commands: %u", dhover->count);
    ImGui::TextDisabled("(submit time exact; width = count, not real RDP time)");
    ImGui::EndTooltip();
  }

  drawCacheTooltip(data, chover);

  // Hover tooltip: function name, duration, % of frame.
  if(hover) {
    string name = prof.labelFor(hover->funcAddr);
    char dbuf[48]; fmtTimeCyc((f64)(hover->end - hover->start), dbuf, sizeof(dbuf));
    f64 pct = 100.0 * (f64)(hover->end - hover->start) / (f64)frameTicks;
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(name.data());
    ImGui::Separator();
    //An ongoing call's width is "so far", not a duration: it has not returned, and
    //if it entered before the window the elapsed time shown is truncated as well.
    if(hover->ongoing) ImGui::Text("running for: %s+  (%.2f%% of window)", dbuf, pct);
    else               ImGui::Text("duration: %s  (%.2f%% of window)", dbuf, pct);
    ImGui::Text("depth: %u", hover->depth);
    if(hover->ongoing) ImGui::TextDisabled("(still on the call stack)");
    ImGui::EndTooltip();
  }

  ImGui::End();
  settings.general.showFlameChart = true;
}

}  // namespace ares::ui
