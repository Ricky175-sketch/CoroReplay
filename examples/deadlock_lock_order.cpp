#include <scenarios.hpp>

#include <cororeplay/async_mutex.hpp>

namespace cororeplay::examples {
namespace {

task<void> lock_pair(async_mutex &first, async_mutex &second) {
  co_await first.lock();
  co_await yield();
  co_await second.lock();
  second.unlock();
  first.unlock();
  co_return;
}

} // namespace

Scenario deadlock_lock_order() {
  return {"deadlock_lock_order", 1, [](Runtime &runtime) {
            auto &left = runtime.make_mutex();
            auto &right = runtime.make_mutex();
            runtime.spawn(lock_pair(left, right));
            runtime.spawn(lock_pair(right, left));
          }};
}

} // namespace cororeplay::examples
