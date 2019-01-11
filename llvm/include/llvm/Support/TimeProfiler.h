//===- llvm/Support/TimeProfiler.h - Hierarchical Time Profiler -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_TIME_PROFILER_H
#define LLVM_SUPPORT_TIME_PROFILER_H

#include "llvm/Support/raw_ostream.h"

namespace llvm {

struct TimeTraceProfiler;
extern TimeTraceProfiler *TimeTraceProfilerInstance;


void TimeTraceProfilerInitialize();
void TimeTraceProfilerCleanup();
void TimeTraceProfilerWrite(std::unique_ptr<raw_pwrite_stream>& OS);
void TimeTraceProfilerBegin(const char *name, const char *detail);
void TimeTraceProfilerEnd();
inline bool TimeTraceProfilerEnabled() { return TimeTraceProfilerInstance != nullptr; }

struct TimeTraceScope {
  TimeTraceScope(const char *name, const char *detail) {
    if (TimeTraceProfilerInstance != nullptr)
      TimeTraceProfilerBegin(name, detail);
  }
  ~TimeTraceScope() {
    if (TimeTraceProfilerInstance != nullptr)
      TimeTraceProfilerEnd();
  }
};



} // end namespace llvm

#endif
