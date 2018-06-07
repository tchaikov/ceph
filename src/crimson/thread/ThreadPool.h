// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab

#include <atomic>
#include <condition_variable>
#include <tuple>
#include <type_traits>
#include <boost/lockfree/queue.hpp>
#include <boost/optional.hpp>
#include <core/future.hh>
#include <core/semaphore.hh>

#include "Condition.h"

namespace ceph::thread {

struct WorkItem {
  virtual ~WorkItem() {}
  virtual void process() = 0;
};

template<typename Func, typename T = std::invoke_result_t<Func>>
struct Task final : WorkItem {
  Func func;
  seastar::future_state<T> state;
  ceph::thread::Condition on_done;
public:
  explicit Task(Func&& f)
    : func(std::move(f))
  {}
  void process() override {
    try {
      state.set(func());
    } catch (...) {
      state.set_exception(std::current_exception());
    }
    on_done.notify();
  }
  seastar::future<T> get_future() {
    return on_done.wait().then([this] {
      return seastar::make_ready_future<T>(state.get0(state.get()));
    });
  }
};

/// an engine for scheduling non-seastar tasks from seastar threads
class ThreadPool {
  std::atomic<bool> stop = false;
  std::mutex mutex;
  std::condition_variable cond;
  std::vector<std::thread> threads;
  std::vector<seastar::semaphore> free_slots;
  // please note, each Task has its own ceph::thread::Condition, which
  // possesses a fd, so we should keep the number of WorkItem in-flight under a
  // reasonable limit.
  static constexpr size_t num_queue_size = 128;
  using item_queue_t =
    boost::lockfree::queue<WorkItem*,
			   boost::lockfree::capacity<num_queue_size>>;
  item_queue_t pending;

  std::vector<std::thread> create_threads(size_t n);
  void loop();
  bool stopping() const {
    return stop.load(std::memory_order_relaxed);
  }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
public:
  explicit ThreadPool(size_t n);
  ~ThreadPool();
  template<typename Func, typename...Args>
  auto submit(Func&& func, Args&&... args) {
    auto& free_slot = free_slots[seastar::engine().cpu_id()];
    auto packaged = [func=std::move(func),
                     args=std::forward_as_tuple(args...)] {
      return std::apply(std::move(func), std::move(args));
    };
    return free_slot.wait().then([this,
                                  &free_slot,
                                  packaged=std::move(packaged)] {
      auto task = new Task{std::move(packaged)};
      auto fut = task->get_future();
      pending.push(task);
      cond.notify_one();
      return fut.finally([&free_slot, task] {
          free_slot.signal();
          delete task;
        });
    });
  }
};

} // namespace ceph::thread
