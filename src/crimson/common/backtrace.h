// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <boost/container/static_vector.hpp>

#include <seastar/util/backtrace.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/shared_ptr.hh>

// symbolized backtrace
namespace symbolized {

// Represents a call stack of a single thread.
class simple_backtrace {
public:
  using vector_type = boost::container::static_vector<seastar::frame, 64>;
private:
  vector_type _frames;
  size_t _hash;
private:
  size_t calculate_hash() const;
public:
  simple_backtrace() = default;
  simple_backtrace(vector_type f) : _frames(std::move(f)) {}
  size_t hash() const { return _hash; }

  friend std::ostream& operator<<(std::ostream& out, const simple_backtrace&);

  bool operator==(const simple_backtrace& o) const {
    return _hash == o._hash && _frames == o._frames;
  }

  bool operator!=(const simple_backtrace& o) const {
    return !(*this == o);
  }
};

using shared_backtrace = seastar::lw_shared_ptr<simple_backtrace>;

// use symbolized::simple_backtrace for main,
// keep using seastar::simple_backtrace for _prev, due to the limitations of
// task::get_backtrace() and seastar::simple_backtrace
class tasktrace {
public:
  using entry = std::variant<seastar::shared_backtrace, seastar::task_entry>;
  using vector_type = boost::container::static_vector<entry, 16>;
private:
  simple_backtrace _main;
  vector_type _prev;
  seastar::scheduling_group _sg;
  size_t _hash;
public:
  tasktrace() = default;
  tasktrace(simple_backtrace main,
            vector_type prev,
            size_t prev_hash,
            seastar::scheduling_group sg);
  ~tasktrace();

  size_t hash() const { return _hash; }

  friend std::ostream& operator<<(std::ostream& out, const tasktrace&);

  bool operator==(const tasktrace& o) const;

  bool operator!=(const tasktrace& o) const {
    return !(*this == o);
  }
};
} // symbolized

namespace std {

template<>
struct hash<symbolized::simple_backtrace> {
    size_t operator()(const seastar::simple_backtrace& b) const {
        return b.hash();
    }
};

template<>
struct hash<symbolized::tasktrace> {
    size_t operator()(const seastar::tasktrace& b) const {
        return b.hash();
    }
};

} // std

namespace symbolized {

using saved_backtrace = tasktrace;

saved_backtrace current_backtrace() noexcept;

tasktrace current_tasktrace() noexcept;

// Collects backtrace only within the currently executing task.
simple_backtrace current_backtrace_tasklocal() noexcept;

std::ostream& operator<<(std::ostream& out, const tasktrace& b);

namespace internal {

template<class Exc>
class backtraced : public Exc {
public:
  template<typename... Args>
  backtraced(Args&&... args)
    : Exc(std::forward<Args>(args)...),
      backtrace(std::make_shared<std::string>(
        fmt::format("{} Backtrace: {}", Exc::what(), current_backtrace())))
  {}

  virtual const char* what() const noexcept override {
    assert(backtrace);
    return backtrace->c_str();
  }
private:
  std::shared_ptr<std::string> backtrace;
};

} // internal

template <class Exc, typename... Args>
std::exception_ptr make_backtraced_exception_ptr(Args&&... args)
{
  using exc_type = std::decay_t<Exc>;
  static_assert(std::is_base_of<std::exception, exc_type>::value,
                "throw_with_backtrace only works with exception types");
  return std::make_exception_ptr<internal::backtraced<exc_type>>(
    Exc(std::forward<Args>(args)...));
}

template <class Exc, typename... Args>
[[noreturn]]
void
throw_with_backtrace(Args&&... args) {
  std::rethrow_exception(
    make_backtraced_exception_ptr<Exc>(std::forward<Args>(args)...));
};

} // symbolized
