// Flame chart: the Cache lane. One bar per cache line transfer (icache fill,
// dcache fill, dcache write-back) with the line's utilisation drawn into it,
// conflict-miss markers and warning triangles, and the tooltip that explains a
// transfer: what was fetched (disassembly + source lines), who caused it, why
// it was needed and who evicted it.
#include "flame-chart.hpp"

#include "../desktop-ui.hpp"

#include <algorithm>
#include <bit>

namespace ares::ui {

using Prof = ares::Nintendo64::CPU::Profiler;

// "0-1, 6-9" style listing of the set bits of a cache-line usage mask.
static auto maskRanges(u32 mask, u32 cells) -> string {
  string out;
  for(u32 i = 0; i < cells; i++) {
    if(!(mask >> i & 1)) continue;
    u32 j = i;
    while(j + 1 < cells && (mask >> (j + 1) & 1)) j++;
    if(out) out.append(", ");
    if(j == i) out.append(i);
    else out.append(i, "-", j);
    i = j;
  }
  return out ? out : string{"none"};
}

// Disassemble one word of a fetched icache line. A private disassembler
// instance (values and colours off) so it never touches live CPU state — the
// emulator's own instance is shared with the tracer on the other thread.
static auto cacheWordText(const CacheEvent& e, u32 word, bool mnemonicOnly) -> string {
  static ares::Nintendo64::CPU::Disassembler dis{ares::Nintendo64::cpu};
  dis.showColors = false;
  dis.showValues = false;
  string text = dis.disassemble(0x8000'0000u | (e.paddr + word * 4), e.words[word]);
  text.strip();
  if(mnemonicOnly) {
    if(auto p = text.find(" ")) text.resize(p());
  } else {
    //collapse the disassembler's mnemonic padding to a single space
    while(text.find("  ")) text.replace("  ", " ");
  }
  return text;
}

// A fill that brought back a line evicted fewer than cacheLines fills ago: a
// same-size cache with a better mapping would still have held it, so this is
// what layout changes can remove.
static auto isConflictFill(const CacheEvent& e) -> bool {
  return e.kind != Prof::CacheDWrite && e.reuseFills && e.reuseFills < Prof::cacheLines;
}

// Severity colour of a conflict fill from its reuse distance, on a log scale
// since 4, 40 and 400 fills apart are the meaningful steps: grey at half the
// cache (barely a conflict), through yellow and orange to red for a line that
// was refetched right after being evicted. Alpha 0 when not a conflict.
static auto conflictColor(const CacheEvent& e) -> ImVec4 {
  if(!isConflictFill(e)) return ImVec4(0, 0, 0, 0);
  const f32 half = Prof::cacheLines / 2.0f;
  f32 t = e.reuseFills >= half ? 0.0f : 1.0f - std::log2((f32)std::max<u32>(e.reuseFills, 1)) / std::log2(half);
  static const ImVec4 stops[] = {
    ImVec4(0.60f, 0.60f, 0.62f, 1),  //grey
    ImVec4(1.00f, 0.82f, 0.24f, 1),  //yellow
    ImVec4(1.00f, 0.55f, 0.16f, 1),  //orange
    ImVec4(1.00f, 0.24f, 0.24f, 1),  //red
  };
  f32 pos = std::clamp(t, 0.0f, 1.0f) * 3.0f;
  u32 a = (u32)pos, b = std::min<u32>(a + 1, 3);
  f32 f = pos - a;
  return ImVec4(stops[a].x + (stops[b].x - stops[a].x) * f, stops[a].y + (stops[b].y - stops[a].y) * f,
                stops[a].z + (stops[b].z - stops[a].z) * f, 1);
}

// Fraction of a resolved cache event's line that was actually used.
static auto cacheUtil(const CacheEvent& e) -> f32 {
  u32 cells = e.kind == Prof::CacheIFill ? 8 : 16;
  u32 used = e.kind == Prof::CacheDWrite ? e.writeMask : (e.readMask | e.writeMask);
  return (f32)std::popcount(used & ((1u << cells) - 1)) / (f32)cells;
}

// Warning triangles collected while drawing the lane. Their base sits above the
// marker strip and the tip may run into the time axis, so they are drawn by
// drawCacheWarnings() once the lanes' clip rect (which stops at the axis) is
// gone. Triangles closer than the spacing fold into the previous one, which
// then shows how many it stands for.
struct WarnTriangle { f32 x, baseY; u32 count; u32 minReuse; };
static std::vector<WarnTriangle> warnTriangles;

auto cacheLaneHeight(f32 rowH) -> f32 {
  return 15.0_px + rowH + 4.0_px;  //label band + bar row + gap
}

// First lane, directly above the CPU call stack so transfers line up with the
// functions below them. icache fills, dcache fills (in) and dcache write-backs
// (out) share the row: the CPU does one at a time, so they never overlap.
// Each bar spans its real duration and is colored by kind. Consecutive
// transfers of the same kind whose bars would touch on screen merge into one
// bar (for drawing and hover); all bars have the same height.
//
// Line utilisation: once a line has been evicted its event knows which bytes
// (words for the icache) were used. A bar wide enough is split into cells in
// address order — used cells in the kind's colour (a distinct tint for bytes
// written), untouched ones a dark shade of it so the kind stays readable. A
// narrower bar is dimmed by how little of its line was used instead. A line
// still in the cache has no verdict yet and is drawn hollow. A conflict fill
// (see isConflictFill) carries a marker strip above the bar, coloured by how
// soon the line came back (conflictColor): contiguous red/orange runs are the
// thrash to look at. The worst ones — back within the configured number of
// fills — also get a warning triangle, thinned out when zoomed out so the
// important misses stay readable.
auto drawCacheLane(const FlameView& view, const FlameData& data, f32 lanesTop) -> CacheHover {
  CacheHover hover;
  auto* dl = view.dl;
  const auto& origin = view.origin;
  const f32 rowH = view.rowH;
  const auto& cacheEvents = data.cacheEvents;
  warnTriangles.clear();  //every frame, even when the lane is scrolled out

  static const ImU32 kindColor[Prof::CacheKinds] = {
    IM_COL32(230, 115, 115, 255),  //icache fill: soft red
    IM_COL32(110, 205, 125, 255),  //dcache fill (in): green
    IM_COL32(100, 155, 240, 255),  //dcache write-back (out): blue
  };
  const ImU32 writtenColor = IM_COL32(100, 155, 240, 255);   //bytes written (will go back out)
  const ImU32 readWriteColor = IM_COL32(130, 220, 220, 255); //bytes both read and written
  auto dim = [](ImU32 c, f32 f) -> ImU32 {
    return IM_COL32((int)(((c >> IM_COL32_R_SHIFT) & 0xff) * f), (int)(((c >> IM_COL32_G_SHIFT) & 0xff) * f),
                    (int)(((c >> IM_COL32_B_SHIFT) & 0xff) * f), 255);
  };
  bool warnICache = settings.general.flameWarnICache;
  bool warnDCache = settings.general.flameWarnDCache;
  const u32 severeFills = settings.general.flameWarnThreshold;

  f32 labelY = lanesTop + 1.0_px;
  f32 rowTop = labelY + 15.0_px;
  f32 cpuTop = lanesTop + cacheLaneHeight(rowH);
  dl->AddLine(ImVec2(origin.x, cpuTop - 2.0_px), ImVec2(origin.x + view.avail.x, cpuTop - 2.0_px), IM_COL32(70, 70, 80, 200));
  if(rowTop <= view.canvasB && rowTop + rowH >= view.clipTop) {
    f32 barBottom = rowTop + rowH - 1.0f;
    f32 barH = (barBottom - (labelY + 2.0_px)) * 0.5f;
    f32 barTop = barBottom - barH;
    const f32 minW = 2.0_px;
    bool mouseInLane = view.hovered && view.mouse.y >= barTop && view.mouse.y <= barBottom;
    f32 lastTriangleX = -1e9f;
    auto warnTriangle = [&](f32 cx, u32 reuse) {
      if(!warnTriangles.empty() && cx - lastTriangleX < 22.0_px) {
        auto& t = warnTriangles.back();
        t.count++;
        if(reuse < t.minReuse) t.minReuse = reuse;
        return;
      }
      lastTriangleX = cx;
      warnTriangles.push_back({cx, barTop - 5.0_px, 1, reuse});
    };
    size_t i = 0;
    while(i < cacheEvents.size()) {
      const auto& e = cacheEvents[i];
      if(e.time > view.viewEnd) break;
      u64 dur = Prof::cacheTicks[e.kind];
      f32 x0 = view.toX(e.time);
      f32 x1 = std::max(view.toX(e.time + dur), x0 + minW);
      size_t j = i + 1;  //absorb following transfers of the same kind that would touch this bar
      u64 lastEnd = e.time + dur;
      f32 utilSum = e.evictTime ? cacheUtil(e) : 0.0f;
      u32 utilN = e.evictTime ? 1 : 0;
      while(j < cacheEvents.size()) {
        const auto& n = cacheEvents[j];
        if(n.kind != e.kind) break;
        f32 nx0 = view.toX(n.time);
        if(nx0 > x1 + 1.0f) break;
        x1 = std::max(x1, std::max(view.toX(n.time + dur), nx0 + minW));
        lastEnd = n.time + dur;
        if(n.evictTime) { utilSum += cacheUtil(n); utilN++; }
        j++;
      }
      u32 count = (u32)(j - i);
      if(x1 >= origin.x && x0 < view.canvasR) {
        f32 cx0 = std::max(x0, origin.x), cx1 = std::min(x1, view.canvasR);
        bool isHover = mouseInLane && !hover.first && view.mouse.x >= cx0 - 1.0_px && view.mouse.x <= cx1 + 1.0_px;
        if(isHover) { hover.first = &e; hover.count = count; hover.lastEnd = lastEnd; }
        u32 cells = e.kind == Prof::CacheIFill ? 8 : 16;
        ImU32 kc = kindColor[e.kind];
        if(count == 1 && e.evictTime && (x1 - x0) >= cells * 2.0_px) {
          //one resolved transfer, wide enough: draw its line cell by cell
          f32 cw = (x1 - x0) / cells;
          ImU32 unused = dim(kc, 0.42f);  //untouched: same hue, much darker
          for(u32 c = 0; c < cells; c++) {
            bool rd = e.readMask >> c & 1, wr = e.writeMask >> c & 1;
            ImU32 col = unused;
            if(e.kind == Prof::CacheDWrite) col = wr ? kc : unused;
            else if(rd && wr)               col = readWriteColor;
            else if(wr)                     col = writtenColor;
            else if(rd)                     col = kc;
            //pixel-snapped cell edges so every separator is a solid 1px column
            f32 a = std::floor(x0 + c * cw), b = std::floor(x0 + (c + 1) * cw);
            a = std::max(a, origin.x); b = std::min(b, view.canvasR);
            if(b <= a) continue;
            dl->AddRectFilled(ImVec2(a, barTop), ImVec2(b, barBottom), col);
            if(cw >= 3.0f && c + 1 < cells && b < view.canvasR)
              dl->AddRectFilled(ImVec2(b - 1.0f, barTop), ImVec2(b, barBottom), IM_COL32(20, 20, 24, 255));
            //instructions of an icache line, once a cell can hold text: the
            //mnemonic first, the operands too when there is room
            if(e.kind == Prof::CacheIFill && e.wordCount == 8 && cw >= 30.0_px && barH >= 10.0_px) {
              string text = cacheWordText(e, c, cw < 96.0_px);
              ImU32 tcol = (rd || wr) ? IM_COL32(15, 15, 18, 255) : IM_COL32(190, 190, 200, 255);
              dl->PushClipRect(ImVec2(a + 1_px, barTop), ImVec2(b - 1_px, barBottom), true);
              dl->AddText(ImVec2(a + 2_px, barTop + (barH - ImGui::GetTextLineHeight()) * 0.5f), tcol, text.data());
              dl->PopClipRect();
            }
          }
        } else if(utilN == 0) {
          //nothing resolved yet: hollow bar in the kind's colour
          dl->AddRectFilled(ImVec2(cx0, barTop), ImVec2(cx1, barBottom), dim(kc, 0.42f));
          dl->AddRect(ImVec2(cx0, barTop), ImVec2(cx1, barBottom), kc);
        } else {
          //too narrow (or several merged): brightness follows the average utilisation
          f32 util = utilSum / utilN;
          dl->AddRectFilled(ImVec2(cx0, barTop), ImVec2(cx1, barBottom), dim(kc, 0.3f + 0.7f * util));
        }
        //conflict markers: a strip above the bar over each conflict fill's extent
        //(merged bars get one per member, so the pattern within stays visible)
        {
          f32 my1 = barTop - 1.0_px, my0 = my1 - 3.0_px;
          bool warn = e.kind == Prof::CacheIFill ? warnICache : warnDCache;
          for(size_t m = i; warn && m < j; m++) {
            const auto& c = cacheEvents[m];
            if(!isConflictFill(c)) continue;
            f32 mx0 = std::max(view.toX(c.time), origin.x);
            f32 mx1 = std::min(std::max(view.toX(c.time + dur), view.toX(c.time) + minW), view.canvasR);
            if(mx1 > mx0) dl->AddRectFilled(ImVec2(mx0, my0), ImVec2(mx1, my1), ImGui::ColorConvertFloat4ToU32(conflictColor(c)));
            if(c.reuseFills < severeFills && mx1 > mx0) warnTriangle((mx0 + mx1) * 0.5f, c.reuseFills);
          }
        }
        if(isHover) dl->AddRect(ImVec2(cx0, barTop), ImVec2(cx1, barBottom), IM_COL32(255, 255, 255, 255));
      }
      i = j;
    }
  }
  //label last: keep it readable over the bars
  laneLabel(dl, ImVec2(origin.x + 3_px, labelY + 1_px), IM_COL32(255, 200, 120, 255), "Cache");
  return hover;
}

// Conflict warning triangles: on top of the lanes and allowed to overlap the
// time axis, but never below their own bar. Coloured by how many warnings
// collapsed into each: yellow for one, through orange to red at 32 or more.
auto drawCacheWarnings(const FlameView& view) -> void {
  auto* dl = view.dl;
  for(auto& t : warnTriangles) {
    if(t.baseY < view.origin.y) continue;  //scrolled entirely out of the canvas
    const f32 h = 20.0_px, w = 20.0_px;
    f32 f = std::clamp(std::log2((f32)t.count) / 5.0f, 0.0f, 1.0f);
    ImVec4 c = f < 0.5f ? ImVec4(1.0f, 0.82f + (0.55f - 0.82f) * (f * 2), 0.24f + (0.16f - 0.24f) * (f * 2), 1)
                        : ImVec4(1.0f, 0.55f + (0.24f - 0.55f) * ((f - 0.5f) * 2), 0.16f + (0.24f - 0.16f) * ((f - 0.5f) * 2), 1);
    ImU32 color = ImGui::ColorConvertFloat4ToU32(c);
    dl->AddTriangleFilled(ImVec2(t.x - w * 0.5f, t.baseY), ImVec2(t.x + w * 0.5f, t.baseY), ImVec2(t.x, t.baseY - h), color);
    dl->AddTriangle(ImVec2(t.x - w * 0.5f, t.baseY), ImVec2(t.x + w * 0.5f, t.baseY), ImVec2(t.x, t.baseY - h), IM_COL32(20, 20, 24, 255), 1.5_px);
    if(t.count == 1) {
      //the "!": a stem and a dot, in the outline colour
      dl->AddRectFilled(ImVec2(t.x - 1.0_px, t.baseY - h + 6.0_px), ImVec2(t.x + 1.0_px, t.baseY - 6.0_px), IM_COL32(20, 20, 24, 255));
      dl->AddRectFilled(ImVec2(t.x - 1.0_px, t.baseY - 4.5_px), ImVec2(t.x + 1.0_px, t.baseY - 2.5_px), IM_COL32(20, 20, 24, 255));
    } else {
      //how many warnings this triangle stands for, small, in the wide lower part
      char buf[8];
      snprintf(buf, sizeof(buf), "%u", std::min<u32>(t.count, 99));
      f32 fontSize = 9.0_px;
      ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, buf);
      dl->AddText(ImGui::GetFont(), fontSize, ImVec2(t.x - ts.x * 0.5f, t.baseY - ts.y - 1.0_px), IM_COL32(20, 20, 24, 255), buf);
    }
  }
}

// Tooltip for the hovered bar: kind, cost, the line, who was running, why the
// fill was needed (reuse distance), what the line held and how it was used,
// and who evicted it.
auto drawCacheTooltip(const FlameData& data, const CacheHover& hover) -> void {
  const CacheEvent* chover = hover.first;
  if(!chover) return;
  auto& prof = ares::Nintendo64::cpu.profiler;
  u32 choverCount = hover.count;
  static const char* kindName[Prof::CacheKinds] = {"icache fill", "dcache fill (in)", "dcache write-back (out)"};
  static const char* kindPlural[Prof::CacheKinds] = {"icache fills", "dcache fills (in)", "dcache write-backs (out)"};
  u32 k = chover->kind;
  u32 bytes = Prof::cacheBytes[k];
  ImGui::BeginTooltip();
  char dbuf[48];
  if(choverCount > 1) {
    fmtTimeCyc((f64)(choverCount * Prof::cacheTicks[k]), dbuf, sizeof(dbuf));
    ImGui::Text("%u %s (%u bytes)", choverCount, kindPlural[k], choverCount * bytes);
    ImGui::Text("transfer time: %s", dbuf);
    char sbuf[48]; fmtTimeCyc((f64)(hover.lastEnd - chover->time), sbuf, sizeof(sbuf));
    ImGui::Text("spread over: %s", sbuf);
  } else {
    fmtTimeCyc((f64)Prof::cacheTicks[k], dbuf, sizeof(dbuf));
    ImGui::Text("%s (%u bytes)", kindName[k], bytes);
    ImGui::Text("duration: %s", dbuf);
  }
  if(choverCount > 1) {
    u32 conflicts = 0;
    const CacheEvent* worst = nullptr;
    for(u32 m = 0; m < choverCount; m++) {
      if(!isConflictFill(chover[m])) continue;
      conflicts++;
      if(!worst || chover[m].reuseFills < worst->reuseFills) worst = &chover[m];
    }
    if(conflicts) ImGui::TextColored(conflictColor(*worst), "%u of these are conflict misses (closest: %u fills apart)", conflicts, worst->reuseFills);
  }
  ImGui::Separator();
  if(choverCount > 1) ImGui::TextDisabled("first of this bar:");
  char tbuf[48]; fmtTimeCyc((f64)chover->time, tbuf, sizeof(tbuf));
  ImGui::Text("at: %s", tbuf);
  ImGui::Text("line: 0x%08X - 0x%08X", chover->paddr, chover->paddr + bytes - 1);
  string target = prof.labelFor(0x8000'0000u | chover->paddr);
  ImGui::Text(k == Prof::CacheIFill ? "fetched code: %s" : "data at: %s", target.data());
  string running = chover->funcAddr ? prof.labelFor(chover->funcAddr) : string{"(no tracked call)"};
  ImGui::Text("while in: %s", running.data());
  //why the fill was needed: reuse distance since the line was last evicted
  if(k != Prof::CacheDWrite) {
    if(!chover->reuseFills) {
      ImGui::TextDisabled("first fill of this line since profiling started");
    } else {
      char rbuf[48]; fmtTimeCyc((f64)chover->reuseTime, rbuf, sizeof(rbuf));
      string by = chover->prevEvictorFuncAddr ? prof.labelFor(chover->prevEvictorFuncAddr) : string{"(no tracked call)"};
      if(isConflictFill(*chover)) {
        ImGui::TextColored(conflictColor(*chover), "conflict miss: evicted %u fills (%s) ago by %s",
                           chover->reuseFills, rbuf, by.data());
        ImGui::TextDisabled("  a better-mapped cache of this size would still hold it: fixable by layout");
      } else {
        ImGui::Text("capacity miss: evicted %u fills (%s) ago by %s", chover->reuseFills, rbuf, by.data());
        ImGui::TextDisabled("  more than %u fills in between: the working set is too big for the cache", Prof::cacheLines);
      }
    }
  }
  //where in that function: symbol + offset of the triggering instruction
  auto pcLabel = [&](u32 pc) -> string {
    if(auto sym = prof.resolve(pc)) return string{sym->name, "+0x", hex(pc - sym->addr)};
    return string{"0x", hex(pc, 8L)};
  };
  //source location of an address, resolved lazily by addr2line (see SourceLines);
  //"" while unknown or unavailable, so the tooltip simply lacks the line
  auto srcLabel = [&](u32 pc) -> string {
    SourceLoc loc;
    if(!sourceLines.lookup(pc, loc) || !loc.known()) return {};
    string where = {loc.file, ":", loc.line};
    if(loc.inlinedInto) where.append(" (inlined into ", loc.inlinedInto, ")");
    if(loc.text) where.append("   ", loc.text);
    return where;
  };
  if(chover->causePc) {
    ImGui::Text("caused by: %s", pcLabel(chover->causePc).data());
    if(auto src = srcLabel(chover->causePc)) ImGui::TextDisabled("  %s", src.data());
  }
  ImGui::Separator();
  if(!chover->evictTime) {
    ImGui::TextDisabled("still cached: usage not known yet");
  } else if(k == Prof::CacheDWrite) {
    ImGui::Text("written back: %d/16 bytes dirty (%s)", std::popcount((u32)chover->writeMask), maskRanges(chover->writeMask, 16).data());
  } else {
    u32 cells = k == Prof::CacheIFill ? 8 : 16;
    u32 used = (chover->readMask | chover->writeMask) & ((1u << cells) - 1);
    if(k == Prof::CacheIFill) {
      ImGui::Text("executed: %d/8 words (%s)", std::popcount(used), maskRanges(used, 8).data());
    } else {
      ImGui::Text("used: %d/16 bytes", std::popcount(used));
      if(chover->readMask)  ImGui::Text("  read: %s", maskRanges(chover->readMask, 16).data());
      if(chover->writeMask) ImGui::Text("  written: %s", maskRanges(chover->writeMask, 16).data());
      if(!chover->readMask && chover->writeMask) ImGui::TextDisabled("  (write-only: the fill was pure cost)");
    }
    //the line's content: instructions with an executed marker, or data words
    if(chover->wordCount == 8) {
      ImGui::Separator();
      ImGui::PushFont(monoFont);
      //file:line and the source text per word, as addr2line answers (asked on
      //first hover; the column fills in over the next frames)
      bool anyPending = false;
      for(u32 w = 0; w < 8; w++) {
        bool ran = chover->readMask >> w & 1;
        string text = cacheWordText(*chover, w, false);
        string src;
        SourceLoc loc;
        u32 pc = 0x8000'0000u | (chover->paddr + w * 4);
        if(sourceLines.lookup(pc, loc)) {
          if(loc.known()) {
            //just the file name: the tooltip is wide enough as it is
            string file = loc.file;
            while(auto p = file.find("/")) file = file.slice(p() + 1);
            src = {file, ":", loc.line};
            if(loc.inlinedInto) src.append(" (inl)");
            if(loc.text) src.append("  ", loc.text);
          }
        } else if(sourceLines.available()) {
          anyPending = true;
        }
        if(ran) ImGui::Text("+0x%02X  %-26s *  %s", w * 4, text.data(), src.data());
        else    ImGui::TextDisabled("+0x%02X  %-26s    %s", w * 4, text.data(), src.data());
      }
      if(anyPending) ImGui::TextDisabled("resolving source lines...");
      ImGui::PopFont();
    } else if(chover->wordCount == 4) {
      ImGui::Separator();
      ImGui::PushFont(monoFont);
      for(u32 w = 0; w < 4; w++) {
        u32 bytesUsed = (used >> (w * 4)) & 0xf;
        if(bytesUsed) ImGui::Text("+0x%X  $%08X", w * 4, chover->words[w]);
        else          ImGui::TextDisabled("+0x%X  $%08X", w * 4, chover->words[w]);
      }
      ImGui::PopFont();
    }
    u64 evictAbs = chover->evictTime, fillAbs = chover->time + data.winStart;
    char lbuf[48]; fmtTimeCyc((f64)(evictAbs > fillAbs ? evictAbs - fillAbs : 0), lbuf, sizeof(lbuf));
    string evictor = chover->evictorFuncAddr ? prof.labelFor(chover->evictorFuncAddr) : string{"(no tracked call)"};
    ImGui::Text("evicted after %s by: %s", lbuf, evictor.data());
    if(chover->evictorPc) {
      ImGui::Text("  at: %s", pcLabel(chover->evictorPc).data());
      if(auto src = srcLabel(chover->evictorPc)) ImGui::TextDisabled("  %s", src.data());
    }
  }
  ImGui::EndTooltip();
}

}  // namespace ares::ui
