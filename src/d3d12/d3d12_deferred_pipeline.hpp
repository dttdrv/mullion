#pragma once

#include "thread.hpp"
#include "util_env.hpp"
#include <atomic>
#include <deque>
#include <functional>
#include <memory>

namespace mullion {

// a graphics pipeline's Metal pipeline, made on a compile thread. Creating a D3D12 pipeline converts its shaders, which
// is where what is wrong with them shows, and leaves to these threads Metal's own compile of the pipeline, the most of
// its cost, which a game creating its pipelines before it starts (an engine's pipeline precompile) waits on otherwise.
// The first draw of the pipeline waits for it, or makes it itself when no compile thread has begun it.
// MULLION_D3D12_DEFER_PIPELINES, read once: 0 makes them when the pipeline is created, for A/B runs; draw leaves them
// to the first draw of each, with no compile threads (d3d12_deferred_pipelines)
class DeferredPipeline {
public:
  explicit DeferredPipeline(std::function<void()> make) : make_(std::move(make)) {}

  // the Metal pipeline is made: made now on this thread when no compile thread has begun it
  void
  Wait() {
    if (done_.load(std::memory_order_acquire))
      return;
    std::unique_lock lock(mutex_);
    if (state_ == State::Queued) {
      state_ = State::Running;
      lock.unlock();
      Make();
      return;
    }
    made_.wait(lock, [&] { return state_ == State::Done; });
  }

  // the pipeline it makes goes: a compile that has not begun is dropped, and one under way finished
  void
  Cancel() {
    std::unique_lock lock(mutex_);
    if (state_ == State::Queued) {
      state_ = State::Done;
      make_ = nullptr;
      done_.store(true, std::memory_order_release);
      return;
    }
    made_.wait(lock, [&] { return state_ == State::Done; });
  }

  // a compile thread's turn: nothing when a draw has taken it already, or the pipeline is gone
  void
  Run() {
    {
      std::lock_guard lock(mutex_);
      if (state_ != State::Queued)
        return;
      state_ = State::Running;
    }
    Make();
  }

  static bool
  Enabled() {
    return Mode() != "0";
  }

  // queue it for the compile threads: one fewer than the CPUs, below normal priority, so the threads that create and
  // draw keep theirs
  static void
  Queue(std::shared_ptr<DeferredPipeline> pipeline) {
    if (Mode() == "draw")
      return;
    auto &pool = Pool::Get();
    {
      std::lock_guard lock(pool.mutex);
      pool.queue.push_back(std::move(pipeline));
    }
    pool.queued.notify_one();
  }

private:
  enum class State { Queued, Running, Done };

  static const std::string &
  Mode() {
    static const std::string mode = env::getEnvVar("MULLION_D3D12_DEFER_PIPELINES");
    return mode;
  }

  void
  Make() {
    make_();
    // what it holds (the pipeline object it makes the Metal pipeline of) goes with it
    make_ = nullptr;
    {
      std::lock_guard lock(mutex_);
      state_ = State::Done;
    }
    done_.store(true, std::memory_order_release);
    made_.notify_all();
  }

  struct Pool {
    mullion::mutex mutex;
    mullion::condition_variable queued;
    std::deque<std::shared_ptr<DeferredPipeline>> queue;
    std::vector<mullion::thread> threads;

    // never destroyed: a process may end with pipelines still queued, which nothing waits for then
    static Pool &
    Get() {
      static Pool *pool = [] {
        auto pool = new Pool();
        uint32_t count = std::max<uint32_t>(1, mullion::thread::hardware_concurrency() - 1);
        for (uint32_t i = 0; i < count; i++) {
          pool->threads.emplace_back([pool] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (;;) {
              std::shared_ptr<DeferredPipeline> next;
              {
                std::unique_lock lock(pool->mutex);
                pool->queued.wait(lock, [&] { return !pool->queue.empty(); });
                next = std::move(pool->queue.front());
                pool->queue.pop_front();
              }
              next->Run();
            }
          });
          pool->threads.back().detach();
        }
        return pool;
      }();
      return *pool;
    }
  };

  std::function<void()> make_;
  mullion::mutex mutex_;
  mullion::condition_variable made_;
  State state_ = State::Queued;
  std::atomic_bool done_ = false;
};

} // namespace mullion
