#include <chrono>

#include "ThreadPool.h"
#include "crimson/net/Config.h"

namespace ceph::thread {

ThreadPool::ThreadPool(size_t n)
  : threads{create_threads(n)}
{
  // try to shard the queue evenly
  const auto queue_size = num_queue_size / seastar::smp::count;
  for (size_t i = 0; i < seastar::smp::count; i++) {
    if (i == 0) {
      free_slots.emplace_back(queue_size + num_queue_size % queue_size);
    } else {
      free_slots.emplace_back(queue_size);
    }
  }
}

ThreadPool::~ThreadPool()
{
  stop = true;
  cond.notify_all();
  for (auto& thread : threads) {
    thread.join();
  }
}

std::vector<std::thread> ThreadPool::create_threads(size_t n)
{
  std::vector<std::thread> workers;
  for (size_t i = 0; i < n; i++) {
    workers.emplace_back([this] {
      loop();
    });
  }
  return workers;
}

bool ThreadPool::have_stray_tasks() const
{
  for (auto& free_slot : free_slots) {
    if (free_slot.waiters() > 0) {
      return true;
    }
  }
  return false;
}

void ThreadPool::loop()
{
  using namespace std::chrono_literals;
  for (;;) {
    WorkItem* work_item = nullptr;
    {
      std::unique_lock lock{mutex};
      cond.wait_for(lock,
                    ceph::net::conf.threadpool_empty_queue_max_wait,
                    [this, &work_item] {
        return pending.pop(work_item) || stopping();
      });
    }
    if (work_item) {
      work_item->process();
    } else if (stopping()) {
      if (!have_stray_tasks()) {
        // keep running for waiting for tasks outside of the queue if any,
        // otherwise say goodbye
        break;
      }
    }
  }
}

}
