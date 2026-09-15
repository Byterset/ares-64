// Flame chart data collection: reads the CPU profiler's span ring, the RSP and
// RDP capture timelines and the cache-event ring into one window-relative
// FlameData. No drawing here; the view (flame-chart.cpp) and any exporter work
// from the result.
#include "flame-chart.hpp"

#include <algorithm>

namespace ares::ui {

auto FlameData::collect(u64 windowTicks, int windowIdx, bool running) -> bool {
  auto& prof = ares::Nintendo64::cpu.profiler;
  auto& rcap = ares::Nintendo64::rsp.capture;
  auto& dcap = ares::Nintendo64::rdp.capture;
  using Span = ares::Nintendo64::CPU::Profiler::Span;

  if(seenGen.empty()) seenGen.resize((size_t)GCols * GDepth, 0);

  u64 viWriteNow = prof.viMarkWrite.load(std::memory_order_acquire);
  bool refresh = !running || !collectedRunning || viWriteNow != collectedViWrite
              || collectedWindowIdx != windowIdx;
  collectedRunning = running;
  if(!refresh) return false;
  collectedViWrite = viWriteNow;
  collectedWindowIdx = windowIdx;

  rightEdge = prof.now();
  if(running && viWriteNow > 0) {
    //pin the right edge to the newest VI (anything newer shows up with the next one)
    u64 lastVi = prof.viMarks[(viWriteNow - 1) % prof.maxViMarks];
    if(lastVi <= rightEdge) rightEdge = lastVi;
  }
  winStart = rightEdge > windowTicks ? rightEdge - windowTicks : 0;
  decimGen++;

  // CPU spans whose [start,end] intersects the window. The ring is appended in
  // end order (now() is monotonic), so walk back from the newest entry and stop
  // at the first span that ends before the window: everything older is outside
  // it. Spans that began before the window clamp their start to 0 (they were
  // already running at the window's left edge).
  spans.clear();
  u64 w = prof.timelineWrite.load(std::memory_order_acquire);
  u64 n = std::min<u64>(w, prof.maxSpans);
  for(u64 i = 0; i < n; i++) {
    const Span& s = prof.timeline[(w - 1 - i) % prof.maxSpans];
    if(s.end < winStart) break;
    if(s.start > rightEdge) continue;  //began after the pinned VI
    if(lastPxPerTick > 0.0) {  //decimate narrow spans to ~1 per pixel per depth
      f64 px0 = ((f64)s.start - (f64)lastViewAbs) * lastPxPerTick;
      f64 px1 = ((f64)s.end   - (f64)lastViewAbs) * lastPxPerTick;
      if(px1 - px0 < 1.5) {
        u32 c = (u32)std::clamp(px0, 0.0, (f64)(GCols - 1));
        u32 d = s.depth < GDepth ? s.depth : GDepth - 1;
        u32& g = seenGen[(size_t)d * GCols + c];
        if(g == decimGen) continue;  //cell already has a span
        g = decimGen;
      }
    }
    CpuSpan t;
    t.funcAddr = s.funcAddr;
    t.depth = s.depth;
    t.isException = s.isException;
    t.start = s.start > winStart ? s.start - winStart : 0;
    t.end   = std::min(s.end, rightEdge) > winStart ? std::min(s.end, rightEdge) - winStart : 0;
    spans.push_back(t);
  }
  ringFull = (n == prof.maxSpans);  //walked the whole ring; oldest may be dropped

  // Calls that are still on the stack. popFrame() is what appends to the ring, so
  // a function that entered before the window (or inside it) and has not returned
  // yet contributes nothing above — the chart would show a hole exactly where the
  // most interesting long-running call is. Synthesise a span per open frame that
  // runs to the right edge (= now). Added after the decimation loop so these are
  // never dropped, and before the sort so they interleave correctly.
  {
    u32 od = prof.openDepth.load(std::memory_order_acquire);
    od = std::min<u32>(od, ares::Nintendo64::CPU::Profiler::maxOpenFrames);
    for(u32 d = 0; d < od; d++) {
      const auto& of = prof.openFrames[d];
      //A pop+push racing this read can hand us a stale slot; drop anything that
      //cannot be a live frame rather than drawing a bar off in the future.
      if(of.start > rightEdge) continue;
      CpuSpan t;
      t.start = of.start > winStart ? of.start - winStart : 0;
      t.end = rightEdge - winStart;
      t.funcAddr = of.funcAddr;
      t.depth = (u16)d;
      t.isException = of.isException;
      t.ongoing = true;
      spans.push_back(t);
    }
  }

  std::sort(spans.begin(), spans.end(),
            [](const CpuSpan& a, const CpuSpan& b) { return a.start < b.start; });
  maxDepth = 0;
  for(auto& s : spans) maxDepth = std::max<u32>(maxDepth, s.depth);

  rspSpans.clear();
  u64 rw = rcap.timelineWrite.load(std::memory_order_acquire);
  u64 rn = std::min<u64>(rw, rcap.maxTimeline);
  for(u64 i = 0; i < rn; i++) {
    const auto& s = rcap.timeline[(rw - 1 - i) % rcap.maxTimeline];
    if(s.end < winStart) break;
    if(s.start > rightEdge) continue;
    u64 rs = s.start > winStart ? s.start - winStart : 0;
    u64 re = std::min(s.end, rightEdge) > winStart ? std::min(s.end, rightEdge) - winStart : 0;
    rspSpans.push_back({rs, re, s.overlayId, s.commandId, s.overhead, s.overheadType});
  }
  std::sort(rspSpans.begin(), rspSpans.end(),
            [](const RspSpan& a, const RspSpan& b) { return a.start < b.start; });

  // RSP hardware halt/break intervals (the bar below the command stream). Closed
  // intervals from the ring, plus the still-open halt drawn live up to the right
  // edge so a currently-stopped RSP shows immediately.
  haltSpans.clear();
  u64 hw = rcap.haltWrite.load(std::memory_order_acquire);
  u64 hn = std::min<u64>(hw, rcap.maxHaltSpans);
  for(u64 i = 0; i < hn; i++) {
    const auto& s = rcap.haltSpans[(hw - 1 - i) % rcap.maxHaltSpans];
    if(s.end < winStart) break;
    if(s.start > rightEdge) continue;
    u64 hs = s.start > winStart ? s.start - winStart : 0;
    u64 he = std::min(s.end, rightEdge) > winStart ? std::min(s.end, rightEdge) - winStart : 0;
    haltSpans.emplace_back(hs, he);
  }
  if(rcap.haltOpen.load(std::memory_order_acquire)) {
    u64 hStart = rcap.haltStartWall.load(std::memory_order_relaxed);
    if(hStart <= rightEdge) {
      u64 hs = hStart > winStart ? hStart - winStart : 0;
      haltSpans.emplace_back(hs, rightEdge - winStart);
    }
  }

  // RDP: one block per DP flush, anchored at its real submit time with a
  // count-proportional synthetic width. Walk back from newest; entries are
  // start-ordered, so once a block's synthetic end falls before the window the
  // rest are too.
  rdpSpans.clear();
  u64 dw = dcap.timelineWrite.load(std::memory_order_acquire);
  u64 dn = std::min<u64>(dw, dcap.maxTimeline);
  for(u64 i = 0; i < dn; i++) {
    const auto& s = dcap.timeline[(dw - 1 - i) % dcap.maxTimeline];
    u64 end = s.start + (u64)s.count * rdpTicksPerCmd;
    if(end < winStart) break;
    if(s.start > rightEdge) continue;
    end = std::min(end, rightEdge);
    u64 ds = s.start > winStart ? s.start - winStart : 0;
    u64 de = end     > winStart ? end     - winStart : 0;
    rdpSpans.push_back({ds, de, s.count});
  }
  std::sort(rdpSpans.begin(), rdpSpans.end(),
            [](const RdpSpan& a, const RdpSpan& b) { return a.start < b.start; });
  // Clamp each flush so it never overruns the next one (synthetic widths can
  // overlap when flushes are dense); keeps blocks readable and start times true.
  for(size_t i = 0; i + 1 < rdpSpans.size(); i++)
    rdpSpans[i].end = std::min(rdpSpans[i].end, rdpSpans[i + 1].start);

  // VI framebuffer-swap markers within the window.
  viMarks.clear();
  u64 vw = prof.viMarkWrite.load(std::memory_order_acquire);
  u64 vn = std::min<u64>(vw, prof.maxViMarks);
  for(u64 i = 0; i < vn; i++) {
    u64 m = prof.viMarks[(vw - 1 - i) % prof.maxViMarks];
    if(m < winStart) break;
    if(m <= rightEdge) viMarks.push_back(m - winStart);
  }

  // Cache line transfers (icache fill, dcache fill / write-back) within the window
  // (time made window-relative). Collected newest-first, then reversed so the
  // lane can be walked in time order.
  cacheEvents.clear();
  u64 cw = prof.cacheEventWrite.load(std::memory_order_acquire);
  u64 cn = prof.cacheEvents.size() == prof.maxCacheEvents ? std::min<u64>(cw, prof.maxCacheEvents) : 0;
  for(u64 i = 0; i < cn; i++) {
    CacheEvent e = prof.cacheEvents[(cw - 1 - i) % prof.maxCacheEvents];
    if(e.time < winStart) break;
    if(e.time > rightEdge) continue;
    e.time -= winStart;
    cacheEvents.push_back(e);
  }
  std::reverse(cacheEvents.begin(), cacheEvents.end());
  cacheRingFull = (cn == prof.maxCacheEvents) && !cacheEvents.empty()
               && cacheEvents.size() == cn;
  return true;
}

}  // namespace ares::ui
