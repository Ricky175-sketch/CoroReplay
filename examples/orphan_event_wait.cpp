#include <scenarios.hpp>

#include <cororeplay/manual_event.hpp>

namespace cororeplay::examples {
namespace {

task<void> waiting_consumer(manual_event &event) { co_await event.wait(); }

task<void> exiting_producer() { co_return; }

} // namespace

Scenario orphan_event_wait() {
  return {"orphan_event_wait", 1, [](Runtime &runtime) {
            auto &event = runtime.make_manual_event();
            runtime.spawn(waiting_consumer(event));
            runtime.spawn(exiting_producer());
          }};
}

} // namespace cororeplay::examples
