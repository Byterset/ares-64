//CPU execution trace for offline instruction-cache analysis (see ExecTrace in
//cpu.hpp). While active, every executed instruction is announced from the
//instruction prologue and every icache fill from Line::fill (via
//profileCacheFill). The interpreter always runs the prologue; the recompiler
//compiles it in because updatePrologueHook() asks for it while tracing, and it
//then refills lines out of line through Line::fill as well.
//
//File layout, all little-endian:
//  header   "P64XTRC1", u32 version, u32 header bytes, u32 line count (512),
//           u32 flags (bit 0: recompiler enabled), u64 CPU cycles at start,
//           u32 per icache line: physical address of its content (~0 = invalid)
//  records  u32 a, u32 b, u32 typeAndCount (type in the top 4 bits)
//Sequential KSEG0 instructions fold into [first, end) range records, and a range
//repeated back to back (a tight loop) folds into a single record's count.

auto CPU::ExecTrace::start(const string& path, u64 limit) -> string {
  if(file) return "a CPU trace is already active; call cpuTraceStop first";
  file = std::fopen(path.data(), "wb");
  if(!file) return {"cannot open CPU trace file: ", path};
  buffer.clear();
  buffer.reserve(bufferBytes + 12);
  stats = {};
  failed = false;
  maxBytes = limit;
  runOpen = false;
  pendingCount = 0;
  uncachedRun = 0;
  startHits = cpu.profile.icacheHits;
  startMisses = cpu.profile.icacheMisses;

  auto put32 = [&](u32 value) { for(u32 i : range(4)) buffer.push_back(u8(value >> i * 8)); };
  for(char c : {'P', '6', '4', 'X', 'T', 'R', 'C', '1'}) buffer.push_back((u8)c);
  put32(version);
  put32(headerBytes);
  put32(512);
  put32(Accuracy::CPU::Recompiler && cpu.recompiler.enabled);
  put32(u32(cpu.profile.cpuCycles));
  put32(u32(u64(cpu.profile.cpuCycles) >> 32));
  //the cache content the trace starts from, so a replay begins in the same state
  for(auto& line : cpu.icache.lines) {
    put32(line.valid() ? (line.tagKey & ~0x0000'0fffu) | line.index : ~0u);
  }
  stats.bytes = buffer.size();
  cpu.updatePrologueHook();
  return {};
}

auto CPU::ExecTrace::stop(Stats& out) -> string {
  if(!file) return "no CPU trace active; call cpuTraceStart first";
  flushAll();
  //totals are written even past maxBytes, so a reader can tell a complete file
  //from a truncated one
  bool truncated = stats.truncated;
  stats.truncated = false;
  maxBytes = 0;
  put(u32(stats.instructions), u32(stats.instructions >> 32), RecordTotals << 28 | TotalInstructions);
  put(u32(stats.fills), u32(stats.fills >> 32), RecordTotals << 28 | TotalFills);
  put(truncated, u32(stats.frames), RecordTotals << 28 | TotalFlags);
  stats.truncated = truncated;
  writeBuffer();
  if(std::fclose(file)) failed = true;
  file = nullptr;
  cpu.updatePrologueHook();
  stats.icacheHits = cpu.profile.icacheHits - startHits;
  stats.icacheMisses = cpu.profile.icacheMisses - startMisses;
  out = stats;
  if(failed) return "error while writing the CPU trace file";
  return {};
}

auto CPU::ExecTrace::onInstruction(u64 address) -> void {
  u32 pc = (u32)address;
  //only KSEG0 (0x80000000-0x9fffffff) executes through the cache
  if((pc >> 29) != 4) {
    closeRun();
    uncachedRun++;
    stats.uncached++;
    return;
  }
  stats.instructions++;
  if(runOpen && pc == runEnd) {
    runEnd = pc + 4;
    return;
  }
  closeRun();
  flushUncached();
  runStart = pc;
  runEnd = pc + 4;
  runOpen = true;
}

auto CPU::ExecTrace::onIcacheFill(u32 lineAddress) -> void {
  stats.fills++;
  //The interpreter fetches (and fills) before it announces an instruction, the
  //recompiler announces first and checks the line afterwards. An instruction
  //already announced from the line being filled belongs behind the fill, which
  //makes both executors serialize the same order.
  bool moved = runOpen && ((runEnd - 4) & 0x1fff'ffe0u) == lineAddress;
  u32 movedPc = runEnd - 4;
  if(moved) {
    runEnd = movedPc;
    if(runEnd == runStart) runOpen = false;
  }
  flushAll();
  put(lineAddress, 0, RecordFill << 28);
  if(moved) {
    runStart = movedPc;
    runEnd = movedPc + 4;
    runOpen = true;
  }
}

auto CPU::ExecTrace::onFrame(u64 frame) -> void {
  if(!file) return;
  flushAll();
  stats.frames++;
  put(u32(frame), u32(cpu.profile.cpuCycles), RecordFrame << 28);
}

auto CPU::ExecTrace::onPower() -> void {
  if(!file) return;
  flushAll();
  put(0, 0, RecordReset << 28);
}

auto CPU::ExecTrace::closeRun() -> void {
  if(!runOpen) return;
  runOpen = false;
  if(pendingCount && pendingStart == runStart && pendingEnd == runEnd && pendingCount < countMask) {
    pendingCount++;
    return;
  }
  flushPendingRange();
  pendingStart = runStart;
  pendingEnd = runEnd;
  pendingCount = 1;
}

auto CPU::ExecTrace::flushPendingRange() -> void {
  if(!pendingCount) return;
  put(pendingStart, pendingEnd, RecordRange << 28 | pendingCount);
  pendingCount = 0;
}

auto CPU::ExecTrace::flushUncached() -> void {
  if(!uncachedRun) return;
  flushPendingRange();  //the held-back range ran before these instructions
  put(uncachedRun, 0, RecordUncached << 28);
  uncachedRun = 0;
}

auto CPU::ExecTrace::flushAll() -> void {
  closeRun();
  flushPendingRange();
  flushUncached();
}

auto CPU::ExecTrace::put(u32 a, u32 b, u32 typeAndCount) -> void {
  if(stats.truncated) return;
  if(maxBytes && stats.bytes + 12 > maxBytes) {
    stats.truncated = true;
    return;
  }
  for(u32 value : {a, b, typeAndCount}) {
    for(u32 i : range(4)) buffer.push_back(u8(value >> i * 8));
  }
  stats.bytes += 12;
  stats.records++;
  if(buffer.size() >= bufferBytes) writeBuffer();
}

auto CPU::ExecTrace::writeBuffer() -> void {
  if(buffer.empty()) return;
  if(std::fwrite(buffer.data(), 1, buffer.size(), file) != buffer.size()) failed = true;
  buffer.clear();
}
