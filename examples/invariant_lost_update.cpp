#include <scenarios.hpp>

#include <memory>

#include <cororeplay/async_mutex.hpp>

namespace cororeplay::examples {
namespace {

struct UpdateState {
  int value = 0;
  int completed = 0;
};

task<void> unsafe_worker(Runtime &runtime, std::shared_ptr<UpdateState> state) {
  const auto snapshot = state->value;
  co_await yield();
  state->value = snapshot + 1;
  ++state->completed;
  if (state->completed == 2) {
    runtime.check_invariant("lost_update", state->value == 2, state->value, 2);
  }
}

task<void> fixed_worker(Runtime &runtime, async_mutex &mutex, std::shared_ptr<UpdateState> state) {
  co_await mutex.lock();
  const auto snapshot = state->value;
  co_await yield();
  state->value = snapshot + 1;
  mutex.unlock();
  ++state->completed;
  if (state->completed == 2) {
    runtime.check_invariant("lost_update", state->value == 2, state->value, 2);
  }
}

} // namespace

Scenario invariant_lost_update(bool fixed) {
  if (fixed) {
    return {"invariant_lost_update_fixed", 1, [](Runtime &runtime) {
              auto state = std::make_shared<UpdateState>();
              auto &mutex = runtime.make_mutex();
              runtime.spawn(fixed_worker(runtime, mutex, state));
              runtime.spawn(fixed_worker(runtime, mutex, std::move(state)));
            }};
  }
  return {"invariant_lost_update", 1, [](Runtime &runtime) {
            auto state = std::make_shared<UpdateState>();
            runtime.spawn(unsafe_worker(runtime, state));
            runtime.spawn(unsafe_worker(runtime, std::move(state)));
          }};
}

} // namespace cororeplay::examples
