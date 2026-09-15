// Source-line lookup for CPU addresses via the toolchain's addr2line, driven by
// the ROM's ELF (libdragon builds with -g, so its DWARF line tables are there).
//
// Everything is asynchronous: lookup() never blocks the UI thread. Unknown
// addresses are queued and resolved on a worker thread in batches, one
// addr2line process per batch; callers simply poll again on later frames.
// The tool is a separate process, so a missing, failing or crashing addr2line
// can never take ares down: it just leaves the location unknown.
#pragma once

#include <nall/nall.hpp>

namespace ares::ui {

struct SourceLoc {
  nall::string function;     //innermost function containing the address (demangled)
  nall::string file;         //path as reported by the tool (may be relative)
  u32 line = 0;              //0 = no line information for this address
  nall::string text;         //that source line, if the file could be found next to the ELF
  nall::string inlinedInto;  //when the code was inlined: the function it was inlined into
  auto known() const -> bool { return line != 0; }
};

struct SourceLines {
  ~SourceLines();

  //new ROM: the ELF to query (empty = none); drops everything cached
  auto setElf(const nall::string& elfPath) -> void;

  //Non-blocking. True with `out` filled when the address has been resolved
  //(possibly as "no information"). False when not yet known: the lookup is
  //queued the first time and answered on a later frame.
  auto lookup(u32 address, SourceLoc& out) -> bool;

  //false once a spawn has failed (tool not installed): lookups then stop
  auto available() -> bool;
  //addresses queued or in flight (for a "resolving..." hint)
  auto pending() -> u32;

private:
  struct Impl;
  Impl* impl = nullptr;
  auto get() -> Impl&;
};

extern SourceLines sourceLines;

}  // namespace ares::ui
