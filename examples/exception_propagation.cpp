#include <scenarios.hpp>

#include <stdexcept>

namespace cororeplay::examples {
namespace {

task<int> failing_child() {
  co_await yield();
  throw std::runtime_error("diagnostic text is intentionally not semantic");
}

task<void> unhandled_parent() {
  const auto value = co_await failing_child();
  (void)value;
}

task<void> catching_parent(Runtime &runtime) {
  bool caught = false;
  try {
    const auto value = co_await failing_child();
    (void)value;
  } catch (const std::runtime_error &) {
    caught = true;
  }
  runtime.check_invariant("child_exception_caught", caught, caught ? 1 : 0, 1);
}

} // namespace

Scenario exception_propagation(bool caught) {
  if (caught) {
    return {"exception_propagation_caught", 1,
            [](Runtime &runtime) { runtime.spawn(catching_parent(runtime)); }};
  }
  return {"exception_propagation", 1, [](Runtime &runtime) { runtime.spawn(unhandled_parent()); }};
}

} // namespace cororeplay::examples
