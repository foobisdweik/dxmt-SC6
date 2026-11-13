#pragma once

#include "thread.hpp"
#include "util_win32_compat.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <queue>
#include <unordered_map>

namespace mullion {

template <typename Task> struct task_trait {
  Task run_task(Task task);
  bool get_done(Task task);
  void set_done(Task task);
};

template <typename Task> class task_scheduler {
public:
  void submit(Task task);

  /**
  SC6 fork: queue a task that nobody waits for yet (for example a pipeline warm-up). Workers run it only when no
  normal task is queued, and at most `max_low_running_` low-priority tasks run at the same time, so warm-up cannot
  delay a task that a frame waits for.
   */
  void submit_low(Task task);

  /**
  SC6 fork: move a task from the low-priority queue to the normal queue, because something now waits for it.
  Returns false if the task already left the low-priority queue (it is running, waiting on a dependency, or done).
   */
  bool promote(Task task);

  task_scheduler();
  ~task_scheduler();

  uint64_t
  get_running_threads() {
    return running.load(std::memory_order_relaxed);
  }

private:
  void worker_func();

  mullion::mutex worker_mutex_;
  mullion::condition_variable worker_cond_;
  std::queue<Task> task_queue_;
  std::queue<Task> task_continuation_queue_;
  std::deque<Task> task_low_queue_;
  uint64_t low_running_ = 0; // protected by worker_mutex_
  uint64_t max_low_running_ = 1;

  mullion::mutex deps_mutex_;
  std::unordered_multimap<Task, Task> task_continuation_;

  std::vector<mullion::thread> workers_;

  std::atomic_bool destroyed = false;
  std::atomic_uint64_t running = 0;
  uint64_t threads;
  uint64_t max_threads;
};

template <typename Task> task_scheduler<Task>::task_scheduler() {
  max_threads = mullion::thread::hardware_concurrency() * 2;
  workers_.reserve(max_threads);
  threads = 2;
  max_low_running_ = std::max(1u, mullion::thread::hardware_concurrency() / 4);

  for (unsigned i = 0; i < threads; i++) {
    workers_.emplace_back([this]() { worker_func(); });
  }
}

template <typename Task> task_scheduler<Task>::~task_scheduler() {
  {
    std::lock_guard<mullion::mutex> lock(worker_mutex_);
    destroyed.store(true);
    // a running task can submit a dependency while the workers are being joined
    max_threads = threads;
  }
  worker_cond_.notify_all();

  for (auto &worker : workers_) {
    if (worker.joinable())
      worker.join();
  }

  workers_.clear();
}

template <typename Task>
void
task_scheduler<Task>::worker_func() {
  struct task_trait<Task> task_trait;
  std::vector<Task> continuation_buffer;
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  while (!destroyed.load()) {
    Task task;
    bool is_low = false;
    {
      std::unique_lock<mullion::mutex> lock(worker_mutex_);

      auto low_available = [this]() { return task_low_queue_.size() && low_running_ < max_low_running_; };

      if (task_queue_.empty() && task_continuation_queue_.empty() && !low_available()) {
        worker_cond_.wait(lock, [&, this]() {
          return task_queue_.size() || task_continuation_queue_.size() || low_available() || destroyed.load();
        });
      }

      if (!task_continuation_queue_.empty()) {
        task = task_continuation_queue_.front();
        task_continuation_queue_.pop();
      } else if (!task_queue_.empty()) {
        task = task_queue_.front();
        task_queue_.pop();
      } else if (low_available()) {
        task = task_low_queue_.front();
        task_low_queue_.pop_front();
        low_running_++;
        is_low = true;
      } else {
        break;
      }
    }
    running.fetch_add(1, std::memory_order_relaxed);
    while (true) {
      Task continuation = task_trait.run_task(task);
      if (continuation == task) {
        {
          std::unique_lock<mullion::mutex> lock(deps_mutex_);
          auto range = task_continuation_.equal_range(continuation);
          for (auto itr = range.first; itr != range.second; ++itr) {
            continuation_buffer.push_back(itr->second);
          }
          task_continuation_.erase(range.first, range.second);
          task_trait.set_done(continuation);
        }
        {
          std::unique_lock<mullion::mutex> lock(worker_mutex_);
          for (auto &task : continuation_buffer) {
            task_continuation_queue_.push(task);
          }
        }
        worker_cond_.notify_all();
        continuation_buffer.clear();
      } else {
        std::unique_lock<mullion::mutex> lock(deps_mutex_);
        // spurious dependency
        if (task_trait.get_done(continuation)) {
          continue;
        }
        task_continuation_.insert({continuation, task});
      }
      break;
    }
    if (is_low) {
      {
        std::unique_lock<mullion::mutex> lock(worker_mutex_);
        low_running_--;
      }
      worker_cond_.notify_all();
    }
    running.fetch_sub(1, std::memory_order_relaxed);
  }
};

template <typename Task>
void
task_scheduler<Task>::submit(Task task) {
  std::unique_lock<mullion::mutex> lock(worker_mutex_);
  task_queue_.push(task);

  if (running.load(std::memory_order_relaxed) == threads && threads < max_threads) {
    workers_.emplace_back([this]() { worker_func(); });
    threads++;
  }

  worker_cond_.notify_one();
}

template <typename Task>
void
task_scheduler<Task>::submit_low(Task task) {
  {
    std::unique_lock<mullion::mutex> lock(worker_mutex_);
    task_low_queue_.push_back(task);
  }
  worker_cond_.notify_one();
}

template <typename Task>
bool
task_scheduler<Task>::promote(Task task) {
  {
    std::unique_lock<mullion::mutex> lock(worker_mutex_);
    auto iter = std::find(task_low_queue_.begin(), task_low_queue_.end(), task);
    if (iter == task_low_queue_.end())
      return false;
    task_low_queue_.erase(iter);
  }
  submit(task);
  return true;
}

}; // namespace mullion
