#pragma once

#include <deque>

#include <cororeplay/task.hpp>

namespace cororeplay {

namespace detail {
class RuntimeImpl;
}

class async_mutex {
public:
  class LockAwaiter {
  public:
    bool await_ready() const;
    template <typename Promise> void await_suspend(std::coroutine_handle<Promise> handle) const {
      handle.promise().mutex_await(*mutex_);
    }
    void await_resume() const noexcept {}

  private:
    explicit LockAwaiter(async_mutex &mutex) : mutex_(&mutex) {}
    async_mutex *mutex_;
    friend class async_mutex;
  };

  async_mutex(const async_mutex &) = delete;
  async_mutex &operator=(const async_mutex &) = delete;

  LockAwaiter lock() { return LockAwaiter(*this); }
  void unlock();
  OperationId owner() const noexcept { return owner_; }

private:
  async_mutex(Runtime &runtime, ResourceId id) : runtime_(&runtime), id_(id) {}

  bool lock_ready();

  Runtime *runtime_;
  ResourceId id_;
  OperationId owner_ = kNoOperation;
  std::deque<OperationId> waiters_;

  friend class Runtime;
  friend class detail::RuntimeImpl;
};

} // namespace cororeplay
