#include <internal/scheduler.hpp>

#include <algorithm>

#include <cororeplay/artifact.hpp>

#include <internal/trace.hpp>

namespace cororeplay::detail {

void Scheduler::run() {
  struct ActiveScope {
    RuntimeImpl &runtime;
    RuntimeImpl *previous = RuntimeImpl::active;
    ActiveScope(RuntimeImpl &value) : runtime(value) {
      RuntimeImpl::active = &value;
      runtime.running = true;
    }
    ~ActiveScope() {
      runtime.running = false;
      RuntimeImpl::active = previous;
    }
  } active_scope(runtime_);
  while (!runtime_.replay_failed && runtime_.record.terminal.kind == FailureKind::none) {
    if (!runtime_.ready.empty() || !runtime_.timer_queue.empty()) {
      if (runtime_.decision_count >= runtime_.config.limits.max_decisions) {
        runtime_.set_terminal(FailureKind::limit_exceeded, "max_decisions");
        break;
      }
      if (!runtime_.ready.empty()) {
        resume_next();
      } else {
        advance_clock();
      }
      continue;
    }

    const auto waiting =
        std::any_of(runtime_.operations.begin(), runtime_.operations.end(),
                    [](const Operation &op) { return op.state == OperationState::waiting; });
    if (waiting) {
      runtime_.set_terminal(runtime_.has_wait_cycle() ? FailureKind::deadlock
                                                      : FailureKind::orphan_wait,
                            runtime_.has_wait_cycle() ? "wait_cycle" : "orphan_wait");
    }
    break;
  }

  runtime_.emit_failure();
  runtime_.active_decision = kNoDecision;
  runtime_.current = kNoOperation;
  runtime_.snapshot_waits();
  runtime_.finish_replay();
}

void Scheduler::resume_next() {
  std::vector<OperationId> ready(runtime_.ready.begin(), runtime_.ready.end());
  OperationId selected = kNoOperation;
  if (runtime_.replay_artifact != nullptr) {
    const auto index = runtime_.decision_count;
    if (index < runtime_.replay_artifact->record.decisions.size()) {
      const auto &expected = runtime_.replay_artifact->record.decisions[index];
      if (expected.kind == DecisionKind::resume) {
        selected = expected.selected_operation;
      }
    }
  } else if (runtime_.config.policy == SchedulingPolicy::fifo) {
    selected =
        *std::min_element(ready.begin(), ready.end(), [this](OperationId left, OperationId right) {
          const auto left_order = runtime_.operation(left).ready_order;
          const auto right_order = runtime_.operation(right).ready_order;
          return left_order == right_order ? left < right : left_order < right_order;
        });
  } else {
    const auto offset = runtime_.random.bounded(static_cast<std::uint64_t>(ready.size()));
    selected = ready[static_cast<std::size_t>(offset)];
  }

  Decision decision;
  decision.index = runtime_.decision_count;
  decision.kind = DecisionKind::resume;
  decision.time = runtime_.clock;
  decision.end_time = runtime_.clock;
  decision.selected_operation = selected;
  decision.ready = ready;
  decision.event_begin = runtime_.record.events.size();
  decision.event_end = decision.event_begin;
  if (!runtime_.begin_decision(std::move(decision))) {
    return;
  }
  if (selected == kNoOperation || runtime_.ready.find(selected) == runtime_.ready.end()) {
    runtime_.divert(ReplayPhase::decision, runtime_.decision_count - 1, "selected_operation",
                    "a member of Ready", std::to_string(selected), selected);
    runtime_.end_decision();
    return;
  }

  runtime_.ready.erase(selected);
  auto &op = runtime_.operation(selected);
  runtime_.current = selected;
  finish_waiting(op);
  op.state = OperationState::running;
  runtime_.emit(EventKind::operation_resume, selected);
  const auto handle = op.handle;
  handle.resume();

  // Attaching a child can reallocate the Operation vector while the coroutine runs.
  auto &resumed = runtime_.operation(selected);
  if (handle.done()) {
    complete(resumed);
  } else {
    if (resumed.state == OperationState::running) {
      runtime_.make_waiting(selected, WaitKind::none);
    }
    runtime_.emit(EventKind::operation_suspend, selected, resumed.wait_for, resumed.resource,
                  resumed.timer, 0, 0, std::string(to_string(resumed.wait_kind)));
  }

  runtime_.current = kNoOperation;
  runtime_.emit_failure();
  runtime_.end_decision();
}

void Scheduler::finish_waiting(Operation &op) {
  if (op.wait_kind == WaitKind::none) {
    return;
  }
  runtime_.emit(EventKind::wait_finish, op.id, op.wait_for, op.resource, op.timer,
                static_cast<std::int64_t>(op.wait_value), 0, std::string(to_string(op.wait_kind)));
  op.wait_kind = WaitKind::none;
  op.wait_for = kNoOperation;
  op.resource = 0;
  op.timer = 0;
  op.wait_value = 0;
}

void Scheduler::complete(Operation &op) {
  op.error = op.exception(op.handle);
  op.state = OperationState::completed;
  runtime_.emit(EventKind::operation_complete, op.id, op.parent, 0, 0, op.error ? 1 : 0);

  if (op.parent != kNoOperation) {
    auto &parent = runtime_.operation(op.parent);
    parent.child = kNoOperation;
    if (parent.state == OperationState::waiting && parent.wait_kind == WaitKind::child &&
        parent.wait_for == op.id) {
      runtime_.make_ready(parent.id, op.id);
    }
  } else if (op.error) {
    runtime_.set_terminal(FailureKind::unhandled_exception, "unhandled_root_exception");
  }
}

void Scheduler::advance_clock() {
  const auto deadline = runtime_.timer_queue.begin()->first;
  Decision decision;
  decision.index = runtime_.decision_count;
  decision.kind = DecisionKind::advance_clock;
  decision.time = runtime_.clock;
  decision.end_time = VirtualTime{deadline};
  decision.event_begin = runtime_.record.events.size();
  decision.event_end = decision.event_begin;
  for (const auto &[time, id] : runtime_.timer_queue) {
    if (time != deadline) {
      break;
    }
    const auto &timer = runtime_.timers[static_cast<std::size_t>(id - 1)];
    decision.wakes.push_back({id, timer.operation});
  }
  const auto wakes = decision.wakes;
  if (!runtime_.begin_decision(std::move(decision))) {
    return;
  }

  runtime_.clock.nanoseconds = deadline;
  runtime_.emit(EventKind::clock_advance);
  for (const auto &wake : wakes) {
    runtime_.timer_queue.erase({deadline, wake.timer});
    auto &timer = runtime_.timers[static_cast<std::size_t>(wake.timer - 1)];
    timer.armed = false;
    runtime_.emit(EventKind::timer_fire, wake.operation, kNoOperation, 0, wake.timer);
    runtime_.make_ready(wake.operation);
  }
  runtime_.emit_failure();
  runtime_.end_decision();
}

} // namespace cororeplay::detail
