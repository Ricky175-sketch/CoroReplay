#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <cororeplay/task.hpp>
#include <cororeplay/time.hpp>

namespace cororeplay {

struct Scenario;
struct Artifact;

namespace detail {
class RuntimeImpl;
}

enum class SchedulingPolicy { fifo, random };
enum class OperationState { created, ready, running, waiting, completed };
enum class WaitKind { none, child, mutex, event, timer };
enum class DecisionKind { resume, advance_clock };
enum class EventKind {
  operation_create,
  operation_resume,
  operation_suspend,
  operation_complete,
  child_attach,
  ready,
  wait_start,
  wait_finish,
  mutex_acquire,
  mutex_handoff,
  mutex_release,
  event_set,
  event_reset,
  timer_arm,
  timer_fire,
  clock_advance,
  invariant,
  failure
};
enum class FailureKind {
  none,
  invariant_violation,
  deadlock,
  orphan_wait,
  limit_exceeded,
  unhandled_exception,
  usage_error,
  virtual_time_overflow,
  replay_divergence
};

std::string_view to_string(OperationState value) noexcept;
std::string_view to_string(WaitKind value) noexcept;
std::string_view to_string(DecisionKind value) noexcept;
std::string_view to_string(EventKind value) noexcept;
std::string_view to_string(FailureKind value) noexcept;

struct RunLimits {
  std::uint64_t max_decisions = 10000;
  std::uint64_t max_operations = 10000;
  std::uint64_t max_trace_events = 100000;
};

struct Event {
  std::uint64_t index = 0;
  std::uint64_t decision_index = kNoDecision;
  VirtualTime time;
  EventKind kind = EventKind::operation_create;
  OperationId operation = kNoOperation;
  OperationId related_operation = kNoOperation;
  ResourceId resource = 0;
  TimerId timer = 0;
  std::int64_t value = 0;
  std::int64_t value2 = 0;
  std::string text;

  bool operator==(const Event &) const = default;
};

struct TimerWake {
  TimerId timer = 0;
  OperationId operation = kNoOperation;
  bool operator==(const TimerWake &) const = default;
};

struct Decision {
  std::uint64_t index = 0;
  DecisionKind kind = DecisionKind::resume;
  VirtualTime time;
  VirtualTime end_time;
  OperationId selected_operation = kNoOperation;
  std::vector<OperationId> ready;
  std::uint64_t event_begin = 0;
  std::uint64_t event_end = 0;
  std::vector<TimerWake> wakes;

  bool operator==(const Decision &) const = default;
};

struct TerminalStatus {
  FailureKind kind = FailureKind::none;
  std::string code = "success";
  bool operator==(const TerminalStatus &) const = default;
};

struct WaitSnapshot {
  OperationId operation = kNoOperation;
  WaitKind wait_kind = WaitKind::none;
  OperationId wait_for = kNoOperation;
  ResourceId resource = 0;
  TimerId timer = 0;
  bool operator==(const WaitSnapshot &) const = default;
};

struct RunRecord {
  std::vector<Decision> decisions;
  std::vector<Event> events;
  TerminalStatus terminal;
  std::vector<WaitSnapshot> waiting;

  bool success() const noexcept { return terminal.kind == FailureKind::none; }
};

struct RuntimeConfig {
  std::uint64_t seed = 0;
  RunLimits limits;
  SchedulingPolicy policy = SchedulingPolicy::random;
  bool trace_enabled = true;
  bool schedule_enabled = true;
};

enum class ReplayPhase { none, metadata, decision, event, schedule, terminal, waiting };

struct Divergence {
  ReplayPhase phase = ReplayPhase::none;
  std::uint64_t index = 0;
  std::string field;
  std::string expected;
  std::string actual;
  OperationId operation = kNoOperation;
  ResourceId resource = 0;
  std::string operation_state;
  std::string resource_state;
  std::vector<Event> common_context;
};

struct ReplayReport {
  bool success = false;
  Divergence divergence;
  TerminalStatus terminal;
  std::uint64_t decisions_replayed = 0;
  std::uint64_t events_replayed = 0;
};

class Runtime {
public:
  explicit Runtime(RuntimeConfig config = {});
  ~Runtime();

  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  Runtime(Runtime &&) = delete;
  Runtime &operator=(Runtime &&) = delete;

  template <typename T> OperationId spawn(task<T> root) { return spawn_handle(root.release()); }

  async_mutex &make_mutex();
  manual_event &make_manual_event();

  bool check_invariant(std::string_view label, bool condition, std::int64_t observed = 0,
                       std::int64_t expected = 0);

  const RunRecord &run();
  const RunRecord &record() const noexcept;
  VirtualTime now() const noexcept;
  OperationState state(OperationId operation) const;

  template <typename T> T result(OperationId operation) {
    rethrow_exception(operation);
    if constexpr (std::is_void_v<T>) {
      (void)take_result_address(operation);
    } else {
      auto *address = static_cast<T *>(take_result_address(operation));
      if (address == nullptr) {
        throw UsageError("operation has no result value");
      }
      return std::move(*address);
    }
  }

private:
  OperationId spawn_handle(detail::TaskHandle root);
  OperationId attach_child(OperationId parent, detail::TaskHandle child);
  void *take_result_address(OperationId operation);
  void rethrow_exception(OperationId operation);

  void yield_current(OperationId operation);
  void sleep_current(OperationId operation, std::uint64_t value, bool until);
  void mutex_wait(OperationId operation, async_mutex &mutex);
  void event_wait(OperationId operation, manual_event &event);
  bool mutex_try_lock(async_mutex &mutex);
  bool event_wait_ready(manual_event &event);
  void mutex_unlock(async_mutex &mutex);
  void event_set(manual_event &event);
  void event_reset(manual_event &event);
  void configure_replay(const Artifact &artifact, Divergence &divergence);

  std::unique_ptr<detail::RuntimeImpl> impl_;

  friend class detail::promise_base;
  friend class async_mutex;
  friend class manual_event;
  friend ReplayReport replay(const Scenario &, const Artifact &);
};

} // namespace cororeplay
