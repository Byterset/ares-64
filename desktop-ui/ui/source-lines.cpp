#include "source-lines.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(PLATFORM_LINUX) || defined(PLATFORM_MACOS) || defined(PLATFORM_BSD)
  #include <sys/wait.h>
  #include <unistd.h>
  #include <fcntl.h>
  #define SOURCE_LINES_HAVE_FORK 1
#else
  #define SOURCE_LINES_HAVE_FORK 0
#endif

namespace ares::ui {

using nall::string;
using nall::file;
using nall::hex;

SourceLines sourceLines;

namespace {

// Run a tool with stdout captured. Returns false if it could not be started at
// all (not installed); `code` is the exit status (128+signal if it crashed).
// Its stderr is discarded. Runs on the worker thread only.
struct ToolResult { bool started = false; int code = -1; string output; };

static auto runTool(const std::vector<string>& argv) -> ToolResult {
  ToolResult result;
#if SOURCE_LINES_HAVE_FORK
  int fd[2];
  if(pipe(fd) == -1) return result;
  pid_t pid = fork();
  if(pid < 0) { close(fd[0]); close(fd[1]); return result; }
  if(pid == 0) {
    //child: stdout -> pipe, stderr -> /dev/null, then exec
    dup2(fd[1], STDOUT_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if(devnull >= 0) dup2(devnull, STDERR_FILENO);
    close(fd[0]); close(fd[1]);
    std::vector<char*> args;
    for(auto& a : argv) args.push_back((char*)a.data());
    args.push_back(nullptr);
    execvp(args[0], args.data());
    _exit(127);  //exec failed: tool not found / not executable
  }
  close(fd[1]);
  std::vector<char> buffer;
  char chunk[4096];
  for(;;) {
    ssize_t n = read(fd[0], chunk, sizeof chunk);
    if(n > 0) { buffer.insert(buffer.end(), chunk, chunk + n); continue; }
    if(n < 0 && errno == EINTR) continue;
    break;
  }
  close(fd[0]);
  int status = 0;
  while(waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  if(WIFEXITED(status)) result.code = WEXITSTATUS(status);
  else if(WIFSIGNALED(status)) result.code = 128 + WTERMSIG(status);
  result.started = result.code != 127;
  buffer.push_back(0);
  result.output = string{buffer.data()};
#endif
  return result;
}

static auto toolPath() -> string {
  if(auto inst = getenv("N64_INST"); inst && *inst) {
    string path = {inst, "/bin/mips64-elf-addr2line"};
    if(file::exists(path)) return path;
  }
  for(auto path : {"/opt/libdragon/bin/mips64-elf-addr2line", "/usr/local/libdragon/bin/mips64-elf-addr2line"}) {
    if(file::exists(path)) return path;
  }
  return "mips64-elf-addr2line";  //via PATH
}

static auto directoryOf(const string& path) -> string {
  s32 slash = -1;
  for(s32 i = 0; i < (s32)path.size(); i++) if(path[i] == '/' || path[i] == '\\') slash = i;
  string dir;
  for(s32 i = 0; i < slash; i++) dir.append(path[i]);
  return dir;
}

}  // namespace

struct SourceLines::Impl {
  std::mutex mutex;
  std::condition_variable wake;
  std::thread worker;
  bool stop = false;
  bool unavailable = false;
  string elfPath;
  u64 generation = 0;  //bumped by setElf so stale batches are discarded

  std::vector<u32> queue;          //addresses waiting for a batch
  std::unordered_set<u32> queued;  //queue + in flight, to avoid duplicates
  u32 inFlight = 0;

  // Resolved addresses. Bounded: a full clear when it grows past the cap is
  // simpler than an LRU and costs nothing but a re-query of whatever is hovered.
  static constexpr u32 maxEntries = 32768;
  std::unordered_map<u32, SourceLoc> cache;

  // Source files, split into lines, keyed by the tool's path. Files that could
  // not be found are cached as empty so they are not searched again. Capped by
  // total bytes; beyond that new files only yield file:line without text.
  static constexpr u64 maxFileBytes = 16ull << 20;
  std::unordered_map<std::string, std::vector<string>> files;
  u64 fileBytes = 0;
  // Where relative source paths were found, keyed by their directory part
  // ("src/scene" -> ".../jam25/engine"): each source directory is located once
  // and reused for every file in it.
  std::unordered_map<std::string, string> baseDirs;

  auto ensureWorker() -> void {
    if(worker.joinable()) return;
    worker = std::thread([this] { run(); });
  }

  auto run() -> void {
    std::unique_lock lock(mutex);
    while(!stop) {
      if(queue.empty() || unavailable) { wake.wait(lock); continue; }
      //take a batch
      std::vector<u32> batch;
      u32 take = std::min<u32>(queue.size(), 256);
      batch.assign(queue.end() - take, queue.end());
      queue.resize(queue.size() - take);
      inFlight = take;
      string elf = elfPath;
      u64 gen = generation;
      lock.unlock();

      auto results = resolve(elf, batch);

      lock.lock();
      inFlight = 0;
      if(gen != generation) { for(auto a : batch) queued.erase(a); continue; }  //ELF changed meanwhile
      if(cache.size() + results.size() > maxEntries) cache.clear();
      for(auto& [addr, loc] : results) cache[addr] = loc;
      for(auto a : batch) {
        queued.erase(a);
        if(!cache.count(a)) cache[a] = SourceLoc{};  //asked, no answer: don't ask again
      }
    }
  }

  // One addr2line run for a batch. -a echoes each address, -f the function,
  // -i the inlining chain (innermost first), -C demangles.
  auto resolve(const string& elf, const std::vector<u32>& batch) -> std::vector<std::pair<u32, SourceLoc>> {
    std::vector<std::pair<u32, SourceLoc>> out;
    if(!elf) return out;
    std::vector<string> argv = {toolPath(), "-e", elf, "-a", "-f", "-i", "-C"};
    for(auto a : batch) argv.push_back({"0x", hex(a, 8L)});
    auto result = runTool(argv);
    if(!result.started) {
      std::lock_guard lock(mutex);
      unavailable = true;
      return out;
    }
    if(result.code != 0) return out;  //failed or crashed: leave this batch unknown

    // Per address: "0x<addr>" then pairs of (function, file:line[ (discriminator n)]).
    auto lines = nall::split(result.output, "\n");
    u32 i = 0;
    while(i < lines.size()) {
      string line = lines[i++].strip();
      if(!line.beginsWith("0x")) continue;
      u32 address = (u32)line.slice(2).hex();
      SourceLoc loc;
      u32 depth = 0;
      while(i + 1 < lines.size() && !lines[i].strip().beginsWith("0x")) {
        string function = lines[i].strip();
        string where = lines[i + 1].strip();
        i += 2;
        if(auto p = where.find(" (discriminator")) where.resize(p());
        //split at the last ':' — file paths may contain ':' on other platforms
        s32 colon = -1;
        for(s32 k = 0; k < (s32)where.size(); k++) if(where[k] == ':') colon = k;
        string file = colon >= 0 ? where.slice(0, colon) : where;
        u32 lineNo = colon >= 0 ? (u32)where.slice(colon + 1).natural() : 0;
        if(depth == 0) {
          if(function != "??") loc.function = function;
          if(file != "??" && lineNo) { loc.file = file; loc.line = lineNo; }
        } else {
          if(function != "??") loc.inlinedInto = function;  //ends on the outermost caller
        }
        depth++;
      }
      if(loc.line) loc.text = sourceText(elf, loc.file, loc.line);
      out.emplace_back(address, std::move(loc));
    }
    return out;
  }

  // The text of file:line, reading the file once. The tool reports the path as
  // compiled (libdragon prefix-maps it to a project-relative one), so try it as
  // given and then relative to the ELF's directory and its parents.
  auto sourceText(const string& elf, const string& file, u32 lineNo) -> string {
    std::string key = file.data();
    {
      std::lock_guard lock(mutex);
      if(auto it = files.find(key); it != files.end()) return lineOf(it->second, lineNo);
    }
    //not seen yet: read it without holding the lock (the UI thread polls under it)
    std::vector<string> content;
    u64 budget = 0;
    { std::lock_guard lock(mutex); budget = maxFileBytes > fileBytes ? maxFileBytes - fileBytes : 0; }
    u64 used = 0;
    string path = locate(elf, file);
    if(path && file::size(path) <= budget) {  //over budget: file:line only
      if(auto data = string::read(path)) {
        used = data.size();
        content = nall::split(data, "\n");
      }
    }
    std::lock_guard lock(mutex);
    fileBytes += used;
    auto it = files.emplace(key, std::move(content)).first;
    return lineOf(it->second, lineNo);
  }

  // Find the file behind a path from the line table. Absolute paths are taken
  // as-is. Relative ones are relative to whatever directory that unit was
  // compiled in (libdragon's -ffile-prefix-map strips it, or replaces it with a
  // short prefix such as "libdragon"), so they can point into the project root,
  // a subdirectory built on its own (an engine), or a library checkout that
  // lives elsewhere entirely ("src/t3d/t3d.c" in the tiny3d repo, "libdragon/
  // src/display.c" next to it). Search order: the ELF's ancestors, then the
  // project's subdirectories, then the siblings of those ancestors and of the
  // toolchain install ($N64_INST). Every hit is remembered per directory.
  auto locate(const string& elf, const string& file) -> string {
    if(file.beginsWith("/") || file.beginsWith("\\") || (file.size() > 1 && file[1] == ':')) {
      return file::exists(file) ? file : string{};
    }
    std::string key = directoryOf(file).data();  //files of one directory share a base
    {
      std::lock_guard lock(mutex);
      if(auto it = baseDirs.find(key); it != baseDirs.end()) {
        string path = {it->second, "/", file};
        if(file::exists(path)) return path;
      }
    }
    auto skip = [](const string& sub) {
      return sub == "build" || sub == ".git" || sub == "filesystem" || sub == "assets"
          || sub == "node_modules" || sub == "cmake-build-debug" || sub.beginsWith(".");
    };
    //1. the ELF's directory and its ancestors
    std::vector<string> roots;
    string dir = directoryOf(elf);
    for(u32 up = 0; up < 6 && dir; up++) { roots.push_back(dir); dir = directoryOf(dir); }
    if(auto inst = getenv("N64_INST"); inst && *inst) { roots.push_back(inst); roots.push_back(directoryOf(inst)); }
    string found;
    for(auto& root : roots) {
      if(file::exists({root, "/", file})) { found = root; break; }
    }
    //2. below the project root (the ELF's parent and grandparent), a few levels
    //   deep and capped, so a huge tree cannot stall the worker
    if(!found && roots.size() >= 2) {
      u32 visited = 0;
      auto walk = [&](auto& self, const string& base, u32 depth) -> bool {
        if(depth > 3 || visited > 400) return false;
        for(auto& name : nall::directory::folders(base)) {
          string sub = name;
          sub.trimRight("/", 1L);
          if(skip(sub)) continue;
          string candidate = {base, "/", sub};
          visited++;
          if(file::exists({candidate, "/", file})) { found = candidate; return true; }
          if(self(self, candidate, depth + 1)) return true;
        }
        return false;
      };
      if(!walk(walk, roots[1], 1) && roots.size() >= 3) walk(walk, roots[2], 1);
    }
    //3. sibling checkouts: one level under each root (a library repo next to
    //   the project, or next to the toolchain install)
    if(!found) {
      u32 checked = 0;
      for(auto& root : roots) {
        if(found || checked > 2000) break;
        for(auto& name : nall::directory::folders(root)) {
          string sub = name;
          sub.trimRight("/", 1L);
          if(skip(sub)) continue;
          checked++;
          if(file::exists({root, "/", sub, "/", file})) { found = {root, "/", sub}; break; }
        }
      }
    }
    if(!found) return {};
    std::lock_guard lock(mutex);
    baseDirs[key] = found;
    return {found, "/", file};
  }

  static auto lineOf(const std::vector<string>& content, u32 lineNo) -> string {
    if(lineNo == 0 || lineNo > content.size()) return {};
    string text = content[lineNo - 1];
    text.strip();
    if(text.size() > 100) { text.resize(97); text.append("..."); }
    return text;
  }
};

SourceLines::~SourceLines() {
  if(!impl) return;
  {
    std::lock_guard lock(impl->mutex);
    impl->stop = true;
  }
  impl->wake.notify_all();
  if(impl->worker.joinable()) impl->worker.join();
  delete impl;
}

auto SourceLines::get() -> Impl& {
  if(!impl) impl = new Impl;
  return *impl;
}

auto SourceLines::setElf(const string& elfPath) -> void {
  auto& s = get();
  std::lock_guard lock(s.mutex);
  s.elfPath = elfPath;
  s.generation++;
  s.queue.clear();
  s.queued.clear();  //in-flight results are discarded by the generation check
  s.cache.clear();
  s.files.clear();
  s.fileBytes = 0;
  s.baseDirs.clear();
  s.unavailable = false;  //a new ROM may live where the toolchain is
}

auto SourceLines::lookup(u32 address, SourceLoc& out) -> bool {
  auto& s = get();
  std::lock_guard lock(s.mutex);
  if(auto it = s.cache.find(address); it != s.cache.end()) { out = it->second; return true; }
  if(s.unavailable || !s.elfPath) return false;
  if(s.queued.insert(address).second) {
    s.queue.push_back(address);
    s.ensureWorker();
    s.wake.notify_one();
  }
  return false;
}

auto SourceLines::available() -> bool {
  auto& s = get();
  std::lock_guard lock(s.mutex);
  return !s.unavailable && (bool)s.elfPath;
}

auto SourceLines::pending() -> u32 {
  auto& s = get();
  std::lock_guard lock(s.mutex);
  return (u32)s.queue.size() + s.inFlight;
}

}  // namespace ares::ui
