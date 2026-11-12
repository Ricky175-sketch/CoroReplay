#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <cororeplay/async_mutex.hpp>
#include <cororeplay/manual_event.hpp>
#include <cororeplay/runtime.hpp>
#include <cororeplay/task.hpp>

namespace cororeplay {
namespace {

RuntimeConfig fifo_config() {
  RuntimeConfig config;
  config.policy = SchedulingPolicy::fifo;
  return config;
}

std::vector<Event> events_of(const RunRecord &record, EventKind kind) {
  std::vector<Event> result;
  for (const auto &event : record.events) {
    if (event.kind == kind) {
      result.push_back(event);
    }
  }
  return result;
}

task<void> fast_path_task(async_mutex &mutex, OperationId &owner, std::vector<std::string> &steps) {
  steps.push_back("before lock");
  co_await mutex.lock();
  owner = mutex.owner();
  steps.push_back("after lock");
  mutex.unlock();
  steps.push_back("after unlock");
}

task<void> holder_task(async_mutex &mutex, manual_event &release, std::vector<std::string> &order,
                       OperationId *owner_after = nullptr) {
  co_await mutex.lock();
  order.push_back("holder");
  co_await release.wait();
  mutex.unlock();
  if (owner_after != nullptr) {
    *owner_after = mutex.owner();
  }
}

task<void> handoff_holder_task(async_mutex &mutex, manual_event &release,
                               OperationId &owner_after) {
  co_await mutex.lock();
  co_await release.wait();
  mutex.unlock();
  owner_after = mutex.owner();
}

task<void> lock_and_record(async_mutex &mutex, std::vector<std::string> &order, std::string label,
                           OperationId *owner_seen = nullptr) {
  co_await mutex.lock();
  order.push_back(std::move(label));
  if (owner_seen != nullptr) {
    *owner_seen = mutex.owner();
  }
  mutex.unlock();
}

task<void> contender_task(async_mutex &mutex, manual_event &release,
                          std::vector<std::string> &order, OperationId &owner_seen) {
  co_await release.wait();
  owner_seen = mutex.owner();
  co_await mutex.lock();
  order.push_back("new contender");
  mutex.unlock();
}

task<void> set_event(manual_event &event) {
  event.set();
  co_return;
}

task<void> self_lock_task(async_mutex &mutex, bool &rejected, OperationId &owner_after) {
  co_await mutex.lock();
  try {
    co_await mutex.lock();
  } catch (const UsageError &) {
    rejected = true;
  }
  owner_after = mutex.owner();
  mutex.unlock();
}

task<void> owner_task(async_mutex &mutex, manual_event &release, OperationId &owner_after,
                      std::vector<std::string> &steps) {
  co_await mutex.lock();
  steps.push_back("owner locked");
  co_await release.wait();
  owner_after = mutex.owner();
  mutex.unlock();
  steps.push_back("owner unlocked");
}

task<void> non_owner_task(async_mutex &mutex, bool &rejected, std::vector<std::string> &steps) {
  steps.push_back("non-owner tried unlock");
  try {
    mutex.unlock();
  } catch (const UsageError &) {
    rejected = true;
  }
  co_return;
}

task<void> foreign_wait_task(async_mutex &mutex, manual_event &event, bool &lock_rejected,
                             bool &unlock_rejected, bool &wait_rejected) {
  try {
    co_await mutex.lock();
  } catch (const UsageError &) {
    lock_rejected = true;
  }
  try {
    mutex.unlock();
  } catch (const UsageError &) {
    unlock_rejected = true;
  }
  try {
    co_await event.wait();
  } catch (const UsageError &) {
    wait_rejected = true;
  }
}

task<void> foreign_set_reset_task(manual_event &event, bool &set_rejected, bool &reset_rejected) {
  try {
    event.set();
  } catch (const UsageError &) {
    set_rejected = true;
  }
  try {
    event.reset();
  } catch (const UsageError &) {
    reset_rejected = true;
  }
  co_return;
}

task<void> set_and_mark(manual_event &event, std::vector<std::string> &steps) {
  steps.push_back("set");
  event.set();
  co_return;
}

task<void> wait_and_mark(manual_event &event, std::vector<std::string> &steps) {
  co_await event.wait();
  steps.push_back("wait returned");
}

task<void> wait_steps(manual_event &event, std::vector<std::string> &steps, std::string label) {
  steps.push_back(label + " waiting");
  co_await event.wait();
  steps.push_back(label + " resumed");
}

task<void> setter_steps(manual_event &event, std::vector<std::string> &steps) {
  steps.push_back("set");
  event.set();
  co_return;
}

task<void> wait_flag(manual_event &event, bool &resumed) {
  co_await event.wait();
  resumed = true;
}

task<void> set_reset_task(manual_event &event) {
  event.set();
  event.reset();
  co_return;
}

task<void> lock_pair(async_mutex &first, async_mutex &second, manual_event &proceed) {
  co_await first.lock();
  co_await proceed.wait();
  co_await second.lock();
  second.unlock();
  first.unlock();
}

task<void> cycle_after_handoff_a(async_mutex &mutex, manual_event &release) {
  co_await mutex.lock();
  co_await release.wait();
  mutex.unlock();
}

task<void> cycle_after_handoff_waiter(async_mutex &owned, async_mutex &needed) {
  co_await owned.lock();
  co_await needed.lock();
}

task<void> wait_only(manual_event &event) { co_await event.wait(); }

task<void> empty_task() { co_return; }

} // namespace

TEST_CASE("async_mutex acquires and releases an uncontended lock on the fast path",
          "[primitives][async_mutex]") {
  OperationId owner_during_lock = kNoOperation;
  std::vector<std::string> steps;
  Runtime runtime(fifo_config());
  auto &mutex = runtime.make_mutex();
  const auto root = runtime.spawn(fast_path_task(mutex, owner_during_lock, steps));

  const auto &record = runtime.run();
  REQUIRE(record.success());
  CHECK(record.decisions.size() == 1);
  CHECK(steps == std::vector<std::string>{"before lock", "after lock", "after unlock"});
  CHECK(owner_during_lock == root);
  CHECK(mutex.owner() == kNoOperation);
  CHECK(runtime.state(root) == OperationState::completed);
  CHECK(events_of(record, EventKind::wait_start).empty());
  CHECK(events_of(record, EventKind::mutex_acquire).size() == 1);
  CHECK(events_of(record, EventKind::mutex_release).size() == 1);
  CHECK(events_of(record, EventKind::mutex_handoff).empty());
}

TEST_CASE("async_mutex hands a contended lock to slow-path waiters in FIFO order",
          "[primitives][async_mutex]") {
  std::vector<std::string> acquisition_order;
  Runtime runtime(fifo_config());
  auto &mutex = runtime.make_mutex();
  auto &release_holder = runtime.make_manual_event();
  const auto holder = runtime.spawn(holder_task(mutex, release_holder, acquisition_order));
  const auto first = runtime.spawn(lock_and_record(mutex, acquisition_order, "first"));
  const auto second = runtime.spawn(lock_and_record(mutex, acquisition_order, "second"));
  runtime.spawn(set_event(release_holder));

  const auto &record = runtime.run();
  REQUIRE(record.success());
  CHECK(acquisition_order == std::vector<std::string>{"holder", "first", "second"});
  const auto handoffs = events_of(record, EventKind::mutex_handoff);
  REQUIRE(handoffs.size() == 2);
  CHECK(handoffs[0].operation == holder);
  CHECK(handoffs[0].related_operation == first);
  CHECK(handoffs[1].operation == first);
  CHECK(handoffs[1].related_operation == second);
  CHECK(runtime.state(holder) == OperationState::completed);
  CHECK(runtime.state(first) == OperationState::completed);
  CHECK(runtime.state(second) == OperationState::completed);
  CHECK(mutex.owner() == kNoOperation);
  CHECK(record.waiting.empty());
}

TEST_CASE("async_mutex logical-owner handoff prevents a ready contender from barging",
          "[primitives][async_mutex]") {
  OperationId owner_after_handoff = kNoOperation;
  OperationId owner_seen_by_contender = kNoOperation;
  OperationId owner_seen_by_queued_waiter = kNoOperation;
  std::vector<std::string> critical_order;
  Runtime runtime(fifo_config());
  auto &mutex = runtime.make_mutex();
  auto &release = runtime.make_manual_event();
  const auto holder = runtime.spawn(handoff_holder_task(mutex, release, owner_after_handoff));
  const auto contender =
      runtime.spawn(contender_task(mutex, release, critical_order, owner_seen_by_contender));
  const auto queued_waiter = runtime.spawn(
      lock_and_record(mutex, critical_order, "queued waiter", &owner_seen_by_queued_waiter));
  runtime.spawn(set_event(release));

  const auto &record = runtime.run();
  REQUIRE(record.success());
  CHECK(owner_after_handoff == queued_waiter);
  CHECK(owner_seen_by_contender == queued_waiter);
  CHECK(owner_seen_by_queued_waiter == queued_waiter);
  CHECK(critical_order == std::vector<std::string>{"queued waiter", "new contender"});
  CHECK(events_of(record, EventKind::mutex_acquire).size() == 1);
  const auto handoffs = events_of(record, EventKind::mutex_handoff);
  REQUIRE(handoffs.size() == 2);
  CHECK(handoffs[0].operation == holder);
  CHECK(handoffs[0].related_operation == queued_waiter);
  CHECK(handoffs[1].operation == queued_waiter);
  CHECK(handoffs[1].related_operation == contender);
  CHECK(mutex.owner() == kNoOperation);
}

TEST_CASE("async_mutex self-lock throws UsageError without releasing the original lock",
          "[primitives][async_mutex][usage]") {
  bool self_lock_rejected = false;
  OperationId owner_after_rejected_self_lock = kNoOperation;
  Runtime runtime(fifo_config());
  auto &mutex = runtime.make_mutex();
  const auto root =
      runtime.spawn(self_lock_task(mutex, self_lock_rejected, owner_after_rejected_self_lock));

  const auto &record = runtime.run();
  REQUIRE(record.success());
  CHECK(self_lock_rejected);
  CHECK(owner_after_rejected_self_lock == root);
  CHECK(mutex.owner() == kNoOperation);
  CHECK(runtime.state(root) == OperationState::completed);
}

TEST_CASE("async_mutex unlock by a non-owner throws UsageError",
          "[primitives][async_mutex][usage]") {
  bool non_owner_unlock_rejected = false;
  OperationId owner_after_rejected_unlock = kNoOperation;
  std::vector<std::string> steps;
  Runtime runtime(fifo_config());
  auto &mutex = runtime.make_mutex();
  auto &release_owner = runtime.make_manual_event();
  const auto owner =
      runtime.spawn(owner_task(mutex, release_owner, owner_after_rejected_unlock, steps));
  runtime.spawn(non_owner_task(mutex, non_owner_unlock_rejected, steps));
  runtime.spawn(set_event(release_owner));

  const auto &record = runtime.run();
  REQUIRE(record.success());
  CHECK(non_owner_unlock_rejected);
  CHECK(owner_after_rejected_unlock == owner);
  CHECK(steps ==
        std::vector<std::string>{"owner locked", "non-owner tried unlock", "owner unlocked"});
  CHECK(mutex.owner() == kNoOperation);
}

TEST_CASE("mutex and event waits reject resources owned by another Runtime",
          "[primitives][usage]") {
  bool foreign_lock_rejected = false;
  bool foreign_unlock_rejected = false;
  bool foreign_wait_rejected = false;
  Runtime owner_runtime(fifo_config());
  Runtime calling_runtime(fifo_config());
  auto &mutex = owner_runtime.make_mutex();
  auto &event = owner_runtime.make_manual_event();
  calling_runtime.spawn(foreign_wait_task(mutex, event, foreign_lock_rejected,
                                          foreign_unlock_rejected, foreign_wait_rejected));

  const auto &record = calling_runtime.run();
  REQUIRE(record.success());
  CHECK(foreign_lock_rejected);
  CHECK(foreign_unlock_rejected);
  CHECK(foreign_wait_rejected);
  CHECK(mutex.owner() == kNoOperation);
  CHECK_FALSE(event.is_set());
  CHECK(event.generation() == 0);
  CHECK(owner_runtime.record().events.empty());
}

TEST_CASE("manual_event set and reset reject callers from another Runtime",
          "[primitives][manual_event][usage]") {
  bool foreign_set_rejected = false;
  bool foreign_reset_rejected = false;
  Runtime owner_runtime(fifo_config());
  Runtime calling_runtime(fifo_config());
  auto &event = owner_runtime.make_manual_event();
  calling_runtime.spawn(
      foreign_set_reset_task(event, foreign_set_rejected, foreign_reset_rejected));

  const auto &record = calling_runtime.run();
  REQUIRE(record.success());
  CHECK(foreign_set_rejected);
  CHECK(foreign_reset_rejected);
  CHECK_FALSE(event.is_set());
  CHECK(event.generation() == 0);
  CHECK(owner_runtime.record().events.empty());
}

TEST_CASE("manual_event set before wait takes the ready fast path", "[primitives][manual_event]") {
  std::vector<std::string> steps;
  Runtime runtime(fifo_config());
  auto &event = runtime.make_manual_event();
  runtime.spawn(set_and_mark(event, steps));
  const auto waiter = runtime.spawn(wait_and_mark(event, steps));

  const auto &record = runtime.run();
  REQUIRE(record.success());
  CHECK(steps == std::vector<std::string>{"set", "wait returned"});
  CHECK(record.decisions.size() == 2);
  CHECK(events_of(record, EventKind::wait_start).empty());
  CHECK(events_of(record, EventKind::event_set).size() == 1);
  CHECK(event.is_set());
  CHECK(event.generation() == 1);
  CHECK(runtime.state(waiter) == OperationState::completed);
}

TEST_CASE("manual_event set wakes every current waiter in FIFO order",
          "[primitives][manual_event]") {
  std::vector<std::string> steps;
  Runtime runtime(fifo_config());
  auto &event = runtime.make_manual_event();
  const auto first = runtime.spawn(wait_steps(event, steps, "first"));
  const auto second = runtime.spawn(wait_steps(event, steps, "second"));
  const auto setter = runtime.spawn(setter_steps(event, steps));

  const auto &record = runtime.run();
  REQUIRE(record.success());
  CHECK(steps == std::vector<std::string>{"first waiting", "second waiting", "set", "first resumed",
                                          "second resumed"});
  CHECK(events_of(record, EventKind::wait_start).size() == 2);
  std::vector<OperationId> woken;
  for (const auto &ready : events_of(record, EventKind::ready)) {
    if (ready.related_operation == setter) {
      woken.push_back(ready.operation);
    }
  }
  CHECK(woken == std::vector<OperationId>{first, second});
  CHECK(record.waiting.empty());
}

TEST_CASE("manual_event reset before a woken waiter resumes does not revoke readiness",
          "[primitives][manual_event]") {
  bool waiter_resumed = false;
  Runtime runtime(fifo_config());
  auto &event = runtime.make_manual_event();
  const auto waiter = runtime.spawn(wait_flag(event, waiter_resumed));
  const auto controller = runtime.spawn(set_reset_task(event));

  const auto &record = runtime.run();
  REQUIRE(record.success());
  CHECK(waiter_resumed);
  CHECK(runtime.state(waiter) == OperationState::completed);
  CHECK(runtime.state(controller) == OperationState::completed);
  CHECK(events_of(record, EventKind::wait_finish).size() == 1);
  CHECK(events_of(record, EventKind::event_reset).size() == 1);
  CHECK_FALSE(event.is_set());
  CHECK(event.generation() == 2);
}

TEST_CASE("lock-order reversal reports a wait cycle with exact final mutex waits",
          "[primitives][async_mutex][deadlock]") {
  Runtime runtime(fifo_config());
  auto &left = runtime.make_mutex();
  auto &right = runtime.make_mutex();
  auto &proceed = runtime.make_manual_event();
  const auto first = runtime.spawn(lock_pair(left, right, proceed));
  const auto second = runtime.spawn(lock_pair(right, left, proceed));
  runtime.spawn(set_event(proceed));

  const auto &record = runtime.run();
  CHECK(record.terminal.kind == FailureKind::deadlock);
  CHECK(record.terminal.code == "wait_cycle");
  const std::vector<WaitSnapshot> expected_waiting = {
      {first, WaitKind::mutex, second, 2, 0},
      {second, WaitKind::mutex, first, 1, 0},
  };
  CHECK(record.waiting == expected_waiting);
  CHECK(left.owner() == first);
  CHECK(right.owner() == second);
}

TEST_CASE("mutex handoff refreshes remaining waiters in the wait-for graph",
          "[primitives][async_mutex][deadlock]") {
  Runtime runtime(fifo_config());
  auto &left = runtime.make_mutex();
  auto &right = runtime.make_mutex();
  auto &release = runtime.make_manual_event();
  runtime.spawn(cycle_after_handoff_a(left, release));
  const auto second = runtime.spawn(cycle_after_handoff_waiter(left, right));
  const auto third = runtime.spawn(cycle_after_handoff_waiter(right, left));
  runtime.spawn(set_event(release));

  const auto &record = runtime.run();
  CHECK(record.terminal.kind == FailureKind::deadlock);
  CHECK(record.terminal.code == "wait_cycle");
  const std::vector<WaitSnapshot> expected_waiting = {
      {second, WaitKind::mutex, third, 2, 0},
      {third, WaitKind::mutex, second, 1, 0},
  };
  CHECK(record.waiting == expected_waiting);
  CHECK(left.owner() == second);
  CHECK(right.owner() == third);
}

TEST_CASE("an unset event with no producer reports an orphan wait and final WaitSnapshot",
          "[primitives][manual_event][orphan]") {
  Runtime runtime(fifo_config());
  auto &event = runtime.make_manual_event();
  const auto waiter = runtime.spawn(wait_only(event));
  const auto producer = runtime.spawn(empty_task());

  const auto &record = runtime.run();
  CHECK(record.terminal.kind == FailureKind::orphan_wait);
  CHECK(record.terminal.code == "orphan_wait");
  const std::vector<WaitSnapshot> expected_waiting = {
      {waiter, WaitKind::event, kNoOperation, 1, 0},
  };
  CHECK(record.waiting == expected_waiting);
  CHECK(runtime.state(waiter) == OperationState::waiting);
  CHECK(runtime.state(producer) == OperationState::completed);

  const auto event_count = record.events.size();
  CHECK_THROWS_AS(event.set(), UsageError);
  CHECK_THROWS_AS(event.reset(), UsageError);
  CHECK_THROWS_AS(runtime.check_invariant("post_run", false), UsageError);
  CHECK(record.events.size() == event_count);
  CHECK(runtime.state(waiter) == OperationState::waiting);
}

} // namespace cororeplay
