#include <internal/trace.hpp>

#include <sstream>

namespace cororeplay {

std::string_view to_string(OperationState value) noexcept {
  switch (value) {
  case OperationState::created:
    return "created";
  case OperationState::ready:
    return "ready";
  case OperationState::running:
    return "running";
  case OperationState::waiting:
    return "waiting";
  case OperationState::completed:
    return "completed";
  }
  return "unknown";
}

std::string_view to_string(WaitKind value) noexcept {
  switch (value) {
  case WaitKind::none:
    return "none";
  case WaitKind::child:
    return "child";
  case WaitKind::mutex:
    return "mutex";
  case WaitKind::event:
    return "event";
  case WaitKind::timer:
    return "timer";
  }
  return "unknown";
}

std::string_view to_string(DecisionKind value) noexcept {
  return value == DecisionKind::resume ? "resume" : "advance_clock";
}

std::string_view to_string(EventKind value) noexcept {
  switch (value) {
  case EventKind::operation_create:
    return "operation_create";
  case EventKind::operation_resume:
    return "operation_resume";
  case EventKind::operation_suspend:
    return "operation_suspend";
  case EventKind::operation_complete:
    return "operation_complete";
  case EventKind::child_attach:
    return "child_attach";
  case EventKind::ready:
    return "ready";
  case EventKind::wait_start:
    return "wait_start";
  case EventKind::wait_finish:
    return "wait_finish";
  case EventKind::mutex_acquire:
    return "mutex_acquire";
  case EventKind::mutex_handoff:
    return "mutex_handoff";
  case EventKind::mutex_release:
    return "mutex_release";
  case EventKind::event_set:
    return "event_set";
  case EventKind::event_reset:
    return "event_reset";
  case EventKind::timer_arm:
    return "timer_arm";
  case EventKind::timer_fire:
    return "timer_fire";
  case EventKind::clock_advance:
    return "clock_advance";
  case EventKind::invariant:
    return "invariant";
  case EventKind::failure:
    return "failure";
  }
  return "unknown";
}

std::string_view to_string(FailureKind value) noexcept {
  switch (value) {
  case FailureKind::none:
    return "success";
  case FailureKind::invariant_violation:
    return "invariant_violation";
  case FailureKind::deadlock:
    return "deadlock";
  case FailureKind::orphan_wait:
    return "orphan_wait";
  case FailureKind::limit_exceeded:
    return "limit_exceeded";
  case FailureKind::unhandled_exception:
    return "unhandled_exception";
  case FailureKind::usage_error:
    return "usage_error";
  case FailureKind::virtual_time_overflow:
    return "virtual_time_overflow";
  case FailureKind::replay_divergence:
    return "replay_divergence";
  }
  return "unknown";
}

namespace detail {

std::string describe_event(const Event &event) {
  std::ostringstream out;
  out << "event#" << event.index << " decision=" << event.decision_index
      << " time=" << event.time.nanoseconds << " kind=" << to_string(event.kind)
      << " op=" << event.operation << " related=" << event.related_operation
      << " resource=" << event.resource << " timer=" << event.timer << " value=" << event.value
      << " value2=" << event.value2 << " text=" << event.text;
  return out.str();
}

std::string describe_terminal(const TerminalStatus &terminal) {
  return std::string(to_string(terminal.kind)) + ":" + terminal.code;
}

std::string describe_wait(const WaitSnapshot &wait) {
  std::ostringstream out;
  out << "op=" << wait.operation << " kind=" << to_string(wait.wait_kind)
      << " wait_for=" << wait.wait_for << " resource=" << wait.resource << " timer=" << wait.timer;
  return out.str();
}

std::string decision_summary(const Decision &decision) {
  std::ostringstream out;
  out << "decision#" << decision.index << " kind=" << to_string(decision.kind)
      << " time=" << decision.time.nanoseconds << " end=" << decision.end_time.nanoseconds
      << " selected=" << decision.selected_operation << " events=[" << decision.event_begin << ','
      << decision.event_end << ") ready=[";
  for (const auto id : decision.ready) {
    out << id << ',';
  }
  out << "] wakes=[";
  for (const auto &wake : decision.wakes) {
    out << wake.timer << ':' << wake.operation << ',';
  }
  out << ']';
  return out.str();
}

} // namespace detail
} // namespace cororeplay
