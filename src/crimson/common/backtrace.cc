// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab

#include "backtrace.h"

#include <signal.h>
#include <iostream>
#include <sstream>
#include <seastar/core/reactor.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/thread_impl.hh>

#include "common/BackTrace.h"   // for BackTrace::demangle()
#include "crimson/common/config_proxy.h"

namespace symbolized {

simple_backtrace current_backtrace_tasklocal() noexcept
{
  simple_backtrace::vector_type v;
  seastar::backtrace([&] (seastar::frame f) {
    if (v.size() < v.capacity()) {
      v.emplace_back(std::move(f));
    }
  });
  return simple_backtrace(std::move(v));
}

size_t simple_backtrace::calculate_hash() const
{
  size_t h = 0;
  for (auto f : _frames) {
    h = ((h << 5) - h) ^ (f.so->begin + f.addr);
  }
  return h;
}

tasktrace::tasktrace(simple_backtrace main, seastar::tasktrace::vector_type prev,
                     size_t prev_hash, seastar::scheduling_group sg)
    : _main(std::move(main))
    , _prev(std::move(prev))
    , _sg(sg)
    , _hash(_main.hash() * 31 ^ prev_hash)
{ }

bool tasktrace::operator==(const tasktrace& o) const {
    return _hash == o._hash && _main == o._main && _prev == o._prev;
}

tasktrace::~tasktrace() {}

// operator<<(std::ostream&, const frame&) is not declared in
// seastar/util/backtrace.hh, but this is not the reason why we are cooking
// our own version. we need to: echo $address | symbolize | demangle 
static void print_frame(std::ostream &out, const seastar::frame &f)
{
  void* const addresses[] = {reinterpret_cast<void*>(f.addr + f.so->begin)};
  // for looking up symbols by their addresses in the dynamic symbol table,
  // please ensure "-rdynamic" is passed to linker
  char** names = backtrace_symbols(addresses, std::size(addresses));
  out << "  ";
  if (names) {
    out << BackTrace::demangle(names[0]) << "\n";
    free(names);
  } else {
    if (!f.so->name.empty()) {
      out << f.so->name << "+";
    }
    out << fmt::format("{:#018x}\n", f.addr);
  }
}

std::ostream& operator<<(std::ostream& out, const simple_backtrace& b)
{
  for (auto f : b._frames) {
    print_frame(out, f);
  }
  return out;
}

std::ostream& operator<<(std::ostream& out, const tasktrace& b)
{
  out << b._main;
  // please note, the frames of defered tasks are not symbolized, and
  // seastar::simple_backtrace does not offer us a simple way to access its
  // _frames, as an alternative, we could parse the output and symbolize
  // them.
  for (auto&& e : b._prev) {
    out << "   --------\n";
    std::visit(seastar::make_visitor([&] (const seastar::shared_backtrace& sb) {
      out << sb;
    }, [&] (const seastar::task_entry& f) {
      out << "   " << f << "\n";
    }), e);
  }
  return out;
}

tasktrace current_tasktrace() noexcept
{
  auto main = current_backtrace_tasklocal();

  seastar::tasktrace::vector_type prev;
  size_t hash = 0;
  if (seastar::engine_is_ready() && seastar::g_current_context) {
    seastar::task* tsk = nullptr;
    // i have to access current thread to checking the waiting tasks
    seastar::thread_context* thread = seastar::thread_impl::get();
    if (thread) {
      tsk = thread->waiting_task();
    } else {
      tsk = seastar::engine().current_task();
    }

    while (tsk && prev.size() < prev.max_size()) {
      seastar::shared_backtrace bt = tsk->get_backtrace();
      hash *= 31;
      if (bt) {
        hash ^= bt->hash();
        prev.push_back(bt);
      } else {
        const std::type_info& ti = typeid(*tsk);
        prev.push_back(seastar::task_entry(ti));
        hash ^= ti.hash_code();
      }
      tsk = tsk->waiting_task();
    }
  }

  return tasktrace(std::move(main), std::move(prev), hash,
                   seastar::current_scheduling_group());
}

saved_backtrace current_backtrace() noexcept
{
  return current_tasktrace();
}

} // symbolized
