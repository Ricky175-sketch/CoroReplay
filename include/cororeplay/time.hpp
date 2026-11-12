#pragma once

#include <chrono>
#include <compare>
#include <cstdint>

#include <cororeplay/task.hpp>

namespace cororeplay {

struct VirtualTime {
  std::uint64_t nanoseconds = 0;
  auto operator<=>(const VirtualTime &) const = default;
};

class SleepAwaitable {
public:
  bool await_ready() const noexcept { return false; }

  template <typename Promise> void await_suspend(std::coroutine_handle<Promise> handle) const {
    handle.promise().sleep_await(value_, until_);
  }

  void await_resume() const noexcept {}

private:
  SleepAwaitable(std::uint64_t value, bool until) : value_(value), until_(until) {}

  std::uint64_t value_;
  bool until_;

  friend SleepAwaitable sleep_for(std::chrono::nanoseconds);
  friend SleepAwaitable sleep_until(VirtualTime);
};

SleepAwaitable sleep_for(std::chrono::nanoseconds duration);
SleepAwaitable sleep_until(VirtualTime deadline);

} // namespace cororeplay
