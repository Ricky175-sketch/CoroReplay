#include <cororeplay/manual_event.hpp>

#include <cororeplay/runtime.hpp>

namespace cororeplay {

bool manual_event::WaitAwaiter::await_ready() const { return event_->wait_ready(); }

bool manual_event::wait_ready() { return runtime_->event_wait_ready(*this); }

void manual_event::set() { runtime_->event_set(*this); }

void manual_event::reset() { runtime_->event_reset(*this); }

} // namespace cororeplay
