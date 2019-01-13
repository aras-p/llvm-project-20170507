//===-- TimeProfiler.cpp - Hierarchical Time Profiler ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file Hierarchical time profiler implementation.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/TimeProfiler.h"
#include "llvm/Support/FileSystem.h"
#include <cassert>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std::chrono;

namespace llvm {

TimeTraceProfiler *TimeTraceProfilerInstance = nullptr;

static std::string EscapeString(const char *src) {
  std::string os;
  while (*src) {
    char c = *src;
    switch (c) {
    case '"':
    case '\\':
    case '\b':
    case '\f':
    case '\n':
    case '\r':
    case '\t':
      os += '\\';
      os += c;
      break;
    default:
      if (c >= 32 && c < 126) {
        os += c;
      }
    }
    ++src;
  }
  return os;
}

typedef duration<steady_clock::rep, steady_clock::period> DurationType;
typedef std::pair<std::string, DurationType> NameAndDuration;

struct Entry {
  time_point<steady_clock> Start;
  DurationType Duration;
  std::string Name;
  std::string Detail;
};

struct TimeTraceProfiler {
  TimeTraceProfiler() {
    Stack.reserve(8);
    Entries.reserve(128);
    StartTime = steady_clock::now();
  }

  void Begin(const std::string &name, const std::string &detail) {
    Entry e = {steady_clock::now(), {}, name, detail};
    Stack.emplace_back(e);
  }

  void End() {
    assert(!Stack.empty() && "Must call Begin first");
    auto &e = Stack.back();
    e.Duration = steady_clock::now() - e.Start;

    // only include sections longer than 500us
    if (duration_cast<microseconds>(e.Duration).count() > 500)
      Entries.emplace_back(e);

    // track total time taken by each "name", but only the topmost levels of them;
    // e.g. if there's a template instantiation that instantiates other templates
    // from within, we only want to add the topmost one.
    // "topmost" happens to be the ones that don't have any currently open
    // entries above itself.
    if (std::find_if(
      ++Stack.rbegin(), Stack.rend(),
      [&](const Entry& val){ return val.Name == e.Name; }) == Stack.rend()) {
      TotalPerName[e.Name] += e.Duration;
      CountPerName[e.Name]++;
    }

    Stack.pop_back();    
  }

  void Write(std::unique_ptr<raw_pwrite_stream> &os) {
    assert(Stack.empty() &&
           "All profiler sections should be ended when calling Write");

    *os << "{ \"traceEvents\": [\n";

    // emit all events for the main flame graph
    for (const auto &e : Entries) {
      auto startUs = duration_cast<microseconds>(e.Start - StartTime).count();
      auto durUs = duration_cast<microseconds>(e.Duration).count();
      *os << "{ \"pid\":1, \"tid\":0, \"ph\":\"X\", \"ts\":" << startUs
          << ", \"dur\":" << durUs << ", \"name\":\""
          << EscapeString(e.Name.c_str()) << "\", \"args\":{ \"detail\":\""
          << EscapeString(e.Detail.c_str()) << "\"} },\n";
    }

    // emit totals by section name as additional "thread" events, sorted from
    // longest one
    int tid = 1;
    std::vector<NameAndDuration> sortedTotals;
    sortedTotals.reserve(TotalPerName.size());
    for (const auto &e : TotalPerName) {
      sortedTotals.push_back(e);
    }
    std::sort(sortedTotals.begin(), sortedTotals.end(), [](const NameAndDuration& a, const NameAndDuration& b) { return a.second > b.second; });
    for (const auto &e : sortedTotals) {
      auto durUs = duration_cast<microseconds>(e.second).count();
      *os << "{ \"pid\":1, \"tid\":" << tid << ", \"ph\":\"X\", \"ts\":" << 0
          << ", \"dur\":" << durUs << ", \"name\":\"Total "
          << EscapeString(e.first.c_str()) << "\", \"args\":{ \"count\":"
          << CountPerName[e.first] <<  ", \"avg ms\":"
          << (durUs / CountPerName[e.first] / 1000)
          << "} },\n";
      ++tid;
    }

    // emit metadata event with process name
    *os << "{ \"cat\":\"\", \"pid\":1, \"tid\":0, \"ts\":0, \"ph\":\"M\", "
           "\"name\":\"process_name\", \"args\":{ \"name\":\"clang\" } }\n";
    *os << "] }\n";
  }

  std::vector<Entry> Stack;
  std::vector<Entry> Entries;
  std::unordered_map<std::string, DurationType> TotalPerName;
  std::unordered_map<std::string, size_t> CountPerName;
  time_point<steady_clock> StartTime;
};

void TimeTraceProfilerInitialize() {
  assert(TimeTraceProfilerInstance == nullptr &&
         "Profiler should not be initialized");
  TimeTraceProfilerInstance = new TimeTraceProfiler();
}

void TimeTraceProfilerCleanup() {
  delete TimeTraceProfilerInstance;
  TimeTraceProfilerInstance = nullptr;
}

void TimeTraceProfilerWrite(std::unique_ptr<raw_pwrite_stream> &OS) {
  assert(TimeTraceProfilerInstance != nullptr &&
         "Profiler object can't be null");
  TimeTraceProfilerInstance->Write(OS);
}

void TimeTraceProfilerBegin(const char *name, const char *detail) {
  if (TimeTraceProfilerInstance != nullptr)
    TimeTraceProfilerInstance->Begin(name, detail);
}

void TimeTraceProfilerEnd() {
  if (TimeTraceProfilerInstance != nullptr)
    TimeTraceProfilerInstance->End();
}

} // namespace llvm
