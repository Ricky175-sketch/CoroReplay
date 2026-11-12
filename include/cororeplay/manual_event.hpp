#pragma once

#include <cstdint>
#include <deque>

#include <cororeplay/task.hpp>

namespace cororeplay {

namespace detail {
class RuntimeImpl;
}

class manual_event {
public:
  class WaitAwaiter {
  public:
    bool await_ready() const;
    template <typename Promise> void await_suspend(std::coroutine_handle<Promise> handle) const {
      handle.promise().event_await(*event_);
    }
    void await_resume() const noexcept {}

  private:
    explicit WaitAwaiter(manual_event &event) : event_(&event) {}
    manual_event *event_;
    friend class manual_event;
  };

  manual_event(const manual_event &) = delete;
  manual_event &operator=(const manual_event &) = delete;

  WaitAwaiter wait() { return WaitAwaiter(*this); }
  void set();
  void reset();
  bool is_set() const noexcept { return set_; }
  std::uint64_t generation() const noexcept { return generation_; }

private:
  manual_event(Runtime &runtime, ResourceId id) : runtime_(&runtime), id_(id) {}
  bool wait_ready();

  Runtime *runtime_;
  ResourceId id_;
  bool set_ = false;
  std::uint64_t generation_ = 0;
  std::deque<OperationId> waiters_;

  friend class Runtime;
  friend class detail::RuntimeImpl;
};

} // namespace cororeplay
