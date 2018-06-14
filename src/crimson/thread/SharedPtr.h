// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <core/future.hh>
#include <core/reactor.hh>
#include <core/shared_ptr.hh>

namespace ceph::thread {

/// a proxy, via which, seastar threads can manage the lifecycle
/// of the proxied object in current thread. and dismiss it when it's
/// not used anymore in this thread.
/// @note @c Proxy<T> is not a smart pointer by itself, it is supposed to
///       be a wrapper of the proxied type
template<class T>
class Proxy : public seastar::enable_lw_shared_from_this<Proxy<T>> {
  using deleter_t = std::function<void(T*)>;
  T* ptr;
  deleter_t deleter;
public:
  Proxy(T* p, deleter_t&& d)
    : deleter(std::move(d))
  {}
  Proxy& operator=(Proxy&& proxy) noexcept {
    std::swap(ptr, proxy.ptr);
    std::swap(deleter, proxy.deleter);
    return *this;
  }
  Proxy(Proxy<T>&& proxy) noexcept
    : ptr{proxy.ptr},
      deleter{std::move(proxy.deleter)} {
    proxy.ptr = nullptr;
  }
  T& get() const noexcept {
    return *ptr;
  }
  ~Proxy() {
    deleter(ptr);
  }
};
  static_assert(std::is_nothrow_move_constructible_v<Proxy<int>>);
// a shared pointer incorporating seastar::lw_shared_ptr<> on
// different shards for allowing seastar threads to access the
// objects allocated on different shared.
template<class T>
class ShardedPtr {
  // keep track of the references used by other cores
  std::vector<seastar::lw_shared_ptr<T>> shared{seastar::smp::count, nullptr};

public:
  // is_default_constructible_v is required by std::map
  ShardedPtr() {}
  ShardedPtr(seastar::lw_shared_ptr<T> ptr) {
    shared[seastar::engine().cpu_id()] = ptr;
  }
  T* get(unsigned from) {
    if (shared[from]) {
      // not optimal, from shard should cache the result by itself
      return shared[from].get();
    } else {
      auto ptr = shared[seastar::engine().cpu_id()];
      shared[from] = ptr;
      return ptr.get();
    }
  }
  void put(unsigned from, T* ptr) {
    assert(shared[from]);
    assert(shared[seastar::engine().cpu_id()].get() == ptr);
    shared[from] = nullptr;
  }
};

} // namespace ceph::thread
