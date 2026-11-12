#include <scenarios.hpp>

namespace cororeplay::examples {
namespace {

task<void> spin_with_suspension() {
  for (;;) {
    co_await yield();
  }
}

} // namespace

Scenario no_progress() {
  return {"no_progress", 1, [](Runtime &runtime) { runtime.spawn(spin_with_suspension()); }};
}

} // namespace cororeplay::examples
