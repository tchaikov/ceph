// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <ostream>
#include <string.h>

#include <boost/stacktrace.hpp>

#include "BackTrace.h"
#include "common/version.h"
#include "common/Formatter.h"

namespace ceph {

BackTrace::BackTrace(int s)
  : bt{s, max}
{}

void BackTrace::print(std::ostream& out) const
{
  out << " " << pretty_version_to_str() << std::endl;
  for (size_t i = 0; i < bt.size(); i++) {
    out << " " << i << ": " << bt[i].name() << std::endl;
  }
}

void BackTrace::dump(Formatter *f) const
{
  f->open_array_section("backtrace");
  for (auto& frame : bt) {
    f->dump_string("frame", frame.name());
  }
  f->close_section();
}

std::string BackTrace::demangle(const char* name)
{
  // find the parentheses and address offset surrounding the mangled name
#ifdef __FreeBSD__
  static constexpr char OPEN = '<';
#else
  static constexpr char OPEN = '(';
#endif
  const char* begin = nullptr;
  const char* end = nullptr;
  for (const char *j = name; *j; ++j) {
    if (*j == OPEN) {
      begin = j + 1;
    } else if (*j == '+') {
      end = j;
    }
  }
  if (begin && end && begin < end) {
    std::string mangled(begin, end);
    int status;
    // only demangle a C++ mangled name
    if (mangled.compare(0, 2, "_Z") == 0) {
      // let __cxa_demangle do the malloc
      char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
      if (!status) {
        std::string full_name{OPEN};
        full_name += demangled;
        full_name += end;
        // buf could be reallocated, so free(demangled) instead
        free(demangled);
        return full_name;
      }
      // demangle failed, just pretend it's a C function with no args
    }
    // C function
    return mangled + "()";
  } else {
    // didn't find the mangled name, just print the whole line
    return name;
  }
}

}
