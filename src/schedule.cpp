#include <cororeplay/artifact.hpp>

#include <algorithm>
#include <internal/scheduler.hpp>
#include <internal/trace.hpp>

namespace cororeplay::detail {

namespace {

template <typename T> std::string scalar(const T &value) {
  if constexpr (std::is_same_v<T, VirtualTime>) {
    return std::to_string(value.nanoseconds);
  } else if constexpr (std::is_enum_v<T>) {
    return std::string(to_string(value));
  } else {
    return std::to_string(value);
  }
}

template <typename T> std::string list(const std::vector<T> &values) {
  std::string result = "[";
  for (const auto &value : values) {
    if constexpr (std::is_same_v<T, TimerWake>) {
      result += std::to_string(value.timer) + ":" + std::to_string(value.operation) + ",";
    } else {
      result += std::to_string(value) + ",";
    }
  }
  return result + "]";
}

} // namespace

void RuntimeImpl::emit(EventKind kind, OperationId op, OperationId related, ResourceId resource,
                       TimerId timer, std::int64_t value, std::int64_t value2, std::string text) {
  if (replay_failed || !config.trace_enabled) {
    return;
  }
  if (record.events.size() >= config.limits.max_trace_events) {
    set_terminal(FailureKind::limit_exceeded, "max_trace_events");
    return;
  }

  Event event;
  event.index = record.events.size();
  event.decision_index = active_decision;
  event.time = clock;
  event.kind = kind;
  event.operation = op;
  event.related_operation = related;
  event.resource = resource;
  event.timer = timer;
  event.value = value;
  event.value2 = value2;
  event.text = std::move(text);

  if (replay_artifact != nullptr) {
    const auto &expected_events = replay_artifact->record.events;
    if (event.index >= expected_events.size()) {
      divert(ReplayPhase::event, event.index, "event_count", "end of events", describe_event(event),
             op, resource);
      return;
    }
    const auto &expected = expected_events[static_cast<std::size_t>(event.index)];
#define COROREPLAY_COMPARE_EVENT(field_name)                                                       \
  if (expected.field_name != event.field_name) {                                                   \
    divert(ReplayPhase::event, event.index, #field_name, scalar(expected.field_name),              \
           scalar(event.field_name), op, resource);                                                \
    return;                                                                                        \
  }
    COROREPLAY_COMPARE_EVENT(index)
    COROREPLAY_COMPARE_EVENT(decision_index)
    COROREPLAY_COMPARE_EVENT(time)
    COROREPLAY_COMPARE_EVENT(kind)
    COROREPLAY_COMPARE_EVENT(operation)
    COROREPLAY_COMPARE_EVENT(related_operation)
    COROREPLAY_COMPARE_EVENT(resource)
    COROREPLAY_COMPARE_EVENT(timer)
    COROREPLAY_COMPARE_EVENT(value)
    COROREPLAY_COMPARE_EVENT(value2)
#undef COROREPLAY_COMPARE_EVENT
    if (expected.text != event.text) {
      divert(ReplayPhase::event, event.index, "text", expected.text, event.text, op, resource);
      return;
    }
  }
  record.events.push_back(std::move(event));
}

void RuntimeImpl::emit_failure() {
  if (failure_emitted || record.terminal.kind == FailureKind::none || replay_failed) {
    return;
  }
  failure_emitted = true;
  emit(EventKind::failure, current, kNoOperation, 0, 0, 0, 0,
       std::string(to_string(record.terminal.kind)) + ":" + record.terminal.code);
}

bool RuntimeImpl::begin_decision(Decision decision) {
  if (replay_failed) {
    return false;
  }
  const auto index = decision_count++;
  if (!config.schedule_enabled) {
    active_decision = decision.index;
    return true;
  }
  if (replay_artifact != nullptr) {
    const auto &expected_decisions = replay_artifact->record.decisions;
    if (index >= expected_decisions.size()) {
      divert(ReplayPhase::schedule, index, "decision_count", "end of decisions",
             decision_summary(decision), decision.selected_operation);
      return false;
    }
    const auto &expected = expected_decisions[index];
    if (expected.index != decision.index) {
      divert(ReplayPhase::decision, index, "index", scalar(expected.index), scalar(decision.index),
             decision.selected_operation);
      return false;
    }
    if (expected.kind != decision.kind) {
      divert(ReplayPhase::decision, index, "kind", scalar(expected.kind), scalar(decision.kind),
             decision.selected_operation);
      return false;
    }
    if (expected.time != decision.time) {
      divert(ReplayPhase::decision, index, "time", scalar(expected.time), scalar(decision.time),
             decision.selected_operation);
      return false;
    }
    if (expected.end_time != decision.end_time) {
      divert(ReplayPhase::decision, index, "end_time", scalar(expected.end_time),
             scalar(decision.end_time), decision.selected_operation);
      return false;
    }
    if (expected.ready != decision.ready) {
      divert(ReplayPhase::decision, index, "ready", list(expected.ready), list(decision.ready),
             decision.selected_operation);
      return false;
    }
    if (expected.selected_operation != decision.selected_operation) {
      divert(ReplayPhase::decision, index, "selected_operation",
             scalar(expected.selected_operation), scalar(decision.selected_operation),
             decision.selected_operation);
      return false;
    }
    if (expected.wakes != decision.wakes) {
      divert(ReplayPhase::decision, index, "wakes", list(expected.wakes), list(decision.wakes),
             decision.selected_operation);
      return false;
    }
    if (expected.event_begin != decision.event_begin) {
      divert(ReplayPhase::decision, index, "event_begin", scalar(expected.event_begin),
             scalar(decision.event_begin), decision.selected_operation);
      return false;
    }
    if (decision.kind == DecisionKind::resume &&
        expected.event_begin < replay_artifact->record.events.size()) {
      const auto &first =
          replay_artifact->record.events[static_cast<std::size_t>(expected.event_begin)];
      if (first.kind == EventKind::operation_resume &&
          first.operation != expected.selected_operation) {
        divert(ReplayPhase::decision, index, "selected_operation", scalar(first.operation),
               scalar(expected.selected_operation), expected.selected_operation);
        return false;
      }
    }
  }
  active_decision = decision.index;
  record.decisions.push_back(std::move(decision));
  return true;
}

void RuntimeImpl::end_decision() {
  if (record.decisions.empty()) {
    active_decision = kNoDecision;
    return;
  }
  auto &actual = record.decisions.back();
  actual.event_end = record.events.size();
  if (!replay_failed && replay_artifact != nullptr) {
    const auto index = record.decisions.size() - 1;
    const auto &expected = replay_artifact->record.decisions[index];
    if (expected.event_end != actual.event_end) {
      divert(ReplayPhase::decision, index, "event_end", scalar(expected.event_end),
             scalar(actual.event_end), actual.selected_operation);
    }
  }
  active_decision = kNoDecision;
}

void RuntimeImpl::divert(ReplayPhase phase, std::uint64_t index, std::string field,
                         std::string expected, std::string actual, OperationId op,
                         ResourceId resource) {
  if (replay_failed || divergence == nullptr) {
    return;
  }
  divergence->phase = phase;
  divergence->index = index;
  divergence->field = std::move(field);
  divergence->expected = std::move(expected);
  divergence->actual = std::move(actual);
  divergence->operation = op != kNoOperation ? op : current;
  divergence->resource = resource;
  divergence->operation_state = operation_description(divergence->operation);
  divergence->resource_state = resource_description(resource);
  const auto begin = record.events.size() > 8 ? record.events.end() - 8 : record.events.begin();
  divergence->common_context.assign(begin, record.events.end());
  replay_failed = true;
}

void RuntimeImpl::finish_replay() {
  if (replay_artifact == nullptr || replay_failed) {
    return;
  }
  const auto &expected = replay_artifact->record;
  if (expected.decisions.size() != record.decisions.size()) {
    divert(ReplayPhase::schedule, record.decisions.size(), "decision_count",
           scalar(expected.decisions.size()), scalar(record.decisions.size()));
    return;
  }
  if (expected.events.size() != record.events.size()) {
    divert(ReplayPhase::schedule, record.events.size(), "event_count",
           scalar(expected.events.size()), scalar(record.events.size()));
    return;
  }
  if (expected.terminal != record.terminal) {
    divert(ReplayPhase::terminal, record.decisions.size(), "terminal",
           describe_terminal(expected.terminal), describe_terminal(record.terminal));
    return;
  }
  if (expected.waiting.size() != record.waiting.size()) {
    divert(ReplayPhase::waiting, 0, "waiting_count", scalar(expected.waiting.size()),
           scalar(record.waiting.size()));
    return;
  }
  for (std::size_t index = 0; index < expected.waiting.size(); ++index) {
    if (expected.waiting[index] != record.waiting[index]) {
      divert(ReplayPhase::waiting, index, "wait_snapshot", describe_wait(expected.waiting[index]),
             describe_wait(record.waiting[index]), record.waiting[index].operation,
             record.waiting[index].resource);
      return;
    }
  }
}

} // namespace cororeplay::detail
