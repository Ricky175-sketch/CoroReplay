#include <cororeplay/time.hpp>

#include <cororeplay/runtime.hpp>

namespace cororeplay {

SleepAwaitable sleep_for(std::chrono::nanoseconds duration) {
  if (duration.count() < 0) {
    throw UsageError("sleep_for duration cannot be negative");
  }
  return SleepAwaitable(static_cast<std::uint64_t>(duration.count()), false);
}

SleepAwaitable sleep_until(VirtualTime deadline) {
  return SleepAwaitable(deadline.nanoseconds, true);
}

} // namespace cororeplay
