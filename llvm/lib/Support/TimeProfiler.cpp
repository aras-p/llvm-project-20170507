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
#include <vector>

using namespace std::chrono;

namespace llvm {
TimeTraceProfiler *TimeTraceProfilerInstance = nullptr;

static void EscapeString(std::string &os, const char *src) {
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
}

struct Entry {
  time_point<steady_clock> Start;
  duration<steady_clock::rep, steady_clock::period> Duration;
  std::string Name;
  std::string Detail;
};

struct TimeTraceProfiler {
  TimeTraceProfiler() {
    Stack.reserve(16);
    Entries.reserve(128);
    StartTime = steady_clock::now();
  }
  ~TimeTraceProfiler() {}

  void Begin(const std::string &name, const std::string &detail) {
    Entry e = {steady_clock::now(), {}, name, detail};
    Stack.emplace_back(e);
  }
  void End() {
    assert(!Stack.empty() && "Must call Begin first");
    auto &e = Stack.back();
    e.Duration = steady_clock::now() - e.Start;
    if (duration_cast<milliseconds>(e.Duration).count() > 10)
      Entries.emplace_back(e);
    Stack.pop_back();
  }
  void Write(std::unique_ptr<raw_pwrite_stream>& os) {
    while (!Stack.empty())
      End();

    *os << "{ \"traceEvents\": [\n";
    *os << "{ \"cat\":\"\", \"pid\":1, \"tid\":0, \"ts\":0, \"ph\":\"M\", "
           "\"name\":\"process_name\", \"args\":{ \"name\":\"clang\" } }\n";
    for (const auto &e : Entries) {
      auto startUs = duration_cast<microseconds>(e.Start - StartTime).count();
      auto durUs = duration_cast<microseconds>(e.Duration).count();
      std::string name, detail;
      EscapeString(name, e.Name.c_str());
      EscapeString(detail, e.Detail.c_str());
      *os << ", { \"pid\":1, \"tid\":0, \"ph\":\"X\", \"ts\":" << startUs
          << ", \"dur\":" << durUs << ", \"name\":\"" << name
          << "\", \"args\":{ \"detail\":\"" << detail << "\"} }\n";
    }
    *os << "] }\n";
  }

  std::vector<Entry> Stack;
  std::vector<Entry> Entries;
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

void TimeTraceProfilerWrite(std::unique_ptr<raw_pwrite_stream>& OS) {
  assert(TimeTraceProfilerInstance != nullptr &&
         "Profiler object can't be null");
  TimeTraceProfilerInstance->Write(OS);
}

void TimeTraceProfilerBegin(const char *name, const char *detail) {
  if (TimeTraceProfilerInstance != nullptr)
    TimeTraceProfilerInstance->Begin(name, detail);
}

void TimeTraceProfilerEnd() {
  if (TimeTraceProfilerInitialize != nullptr)
    TimeTraceProfilerInstance->End();
}

} // namespace llvm
