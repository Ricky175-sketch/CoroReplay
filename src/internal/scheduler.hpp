#pragma once

#include <deque>
#include <set>
#include <string>
#include <vector>

#include <cororeplay/async_mutex.hpp>
#include <cororeplay/manual_event.hpp>
#include <cororeplay/runtime.hpp>

#include <internal/operation.hpp>
#include <internal/random.hpp>

namespace cororeplay::detail {

class RuntimeImpl {
public:
  explicit RuntimeImpl(RuntimeConfig config);
  ~RuntimeImpl();

  Operation &operation(OperationId id);
  const Operation &operation(OperationId id) const;
  OperationId attach(TaskHandle task, OperationId parent);
  void *take_result(OperationId id);
  void rethrow_exception(OperationId id);

  void make_ready(OperationId id, OperationId related = kNoOperation);
  void make_waiting(OperationId id, WaitKind kind, ResourceId resource = 0, TimerId timer = 0,
                    OperationId wait_for = kNoOperation, std::uint64_t value = 0);
  void yield(OperationId id);
  void sleep(OperationId id, std::uint64_t value, bool until);
  bool try_lock(async_mutex &mutex, OperationId id);
  void mutex_wait(async_mutex &mutex, OperationId id);
  void mutex_unlock(async_mutex &mutex, OperationId id);
  bool event_ready(manual_event &event, OperationId id) const;
  void event_wait(manual_event &event, OperationId id);
  void event_set(manual_event &event, OperationId id);
  void event_reset(manual_event &event, OperationId id);

  void invariant(std::string_view label, bool condition, std::int64_t observed,
                 std::int64_t expected);
  void set_terminal(FailureKind kind, std::string code);
  void emit(EventKind kind, OperationId operation = kNoOperation,
            OperationId related = kNoOperation, ResourceId resource = 0, TimerId timer = 0,
            std::int64_t value = 0, std::int64_t value2 = 0, std::string text = {});
  void emit_failure();
  void snapshot_waits();
  bool has_wait_cycle();

  bool begin_decision(Decision decision);
  void end_decision();
  void divert(ReplayPhase phase, std::uint64_t index, std::string field, std::string expected,
              std::string actual, OperationId operation = kNoOperation, ResourceId resource = 0);
  void finish_replay();
  std::string operation_description(OperationId id) const;
  std::string resource_description(ResourceId id) const;

  void require_current(OperationId id) const;
  void require_same_runtime(const Runtime *expected, OperationId id) const;

  RuntimeConfig config;
  Runtime *owner = nullptr;
  VirtualTime clock;
  std::vector<Operation> operations;
  std::vector<std::unique_ptr<async_mutex>> mutexes;
  std::vector<std::unique_ptr<manual_event>> events;
  enum class ResourceKind { mutex, event };
  struct ResourceInfo {
    ResourceKind kind;
    std::size_t index;
  };
  std::vector<ResourceInfo> resources;
  std::vector<Timer> timers;
  std::set<std::pair<std::uint64_t, TimerId>> timer_queue;
  std::set<OperationId> ready;
  std::uint64_t next_ready_order = 0;
  Random random;
  RunRecord record;
  std::uint64_t decision_count = 0;
  OperationId current = kNoOperation;
  std::uint64_t active_decision = kNoDecision;
  bool ran = false;
  bool running = false;
  bool replay_failed = false;
  bool failure_emitted = false;
  const Artifact *replay_artifact = nullptr;
  Divergence *divergence = nullptr;
  static thread_local RuntimeImpl *active;
};

class Scheduler {
public:
  explicit Scheduler(RuntimeImpl &runtime) : runtime_(runtime) {}
  void run();

private:
  void resume_next();
  void advance_clock();
  void complete(Operation &operation);
  void finish_waiting(Operation &operation);

  RuntimeImpl &runtime_;
};

} // namespace cororeplay::detail
