#pragma once

#include <cororeplay/explore.hpp>

namespace cororeplay::examples {

Scenario deadlock_lock_order();
Scenario invariant_lost_update(bool fixed = false);
Scenario orphan_event_wait();
Scenario no_progress();
Scenario exception_propagation(bool caught = false);

} // namespace cororeplay::examples
