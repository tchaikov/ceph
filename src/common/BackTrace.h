// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_BACKTRACE_H
#define CEPH_BACKTRACE_H

#include <iosfwd>
#include <stdlib.h>

#include <boost/stacktrace.hpp>

namespace ceph {

class Formatter;

struct BackTrace {
  boost::stacktrace::stacktrace bt;
  const static int max = 100;

  explicit BackTrace(int s);
  void print(std::ostream& out) const;
  void dump(Formatter *f) const;
  static std::string demangle(const char* name);
};

inline std::ostream& operator<<(std::ostream& out, const BackTrace& bt) {
  bt.print(out);
  return out;
}

}

#endif
