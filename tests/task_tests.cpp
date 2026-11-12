#include <catch2/catch_test_macros.hpp>

#include <cororeplay/manual_event.hpp>
#include <cororeplay/runtime.hpp>
#include <cororeplay/task.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using cororeplay::FailureKind;
using cororeplay::manual_event;
using cororeplay::OperationState;
using cororeplay::Runtime;
using cororeplay::task;
using cororeplay::UsageError;
using cororeplay::WaitKind;

struct LifetimeProbe {
  explicit LifetimeProbe(int &live_count) : live(&live_count) { ++*live; }
  LifetimeProbe(const LifetimeProbe &other) : live(other.live) { ++*live; }
  LifetimeProbe(LifetimeProbe &&other) noexcept : live(other.live) { ++*live; }
  ~LifetimeProbe() { --*live; }

  int *live;
};

task<void> lazy_with_probe(LifetimeProbe probe, int &starts) {
  (void)probe;
  ++starts;
  co_return;
}

task<int> immediate_value(int value) { co_return value; }

struct MoveOnlyResult {
  explicit MoveOnlyResult(int result) : value(result) {}
  MoveOnlyResult() = delete;
  MoveOnlyResult(const MoveOnlyResult &) = delete;
  MoveOnlyResult &operator=(const MoveOnlyResult &) = delete;
  MoveOnlyResult(MoveOnlyResult &&) noexcept = default;
  MoveOnlyResult &operator=(MoveOnlyResult &&) noexcept = default;

  int value;
};

task<MoveOnlyResult> move_only_value() {
  co_await cororeplay::yield();
  co_return MoveOnlyResult{73};
}

task<int> nested_leaf() {
  co_await cororeplay::yield();
  co_return 40;
}

task<int> nested_middle() {
  const auto value = co_await nested_leaf();
  co_return value + 2;
}

task<int> nested_root() {
  const auto value = co_await nested_middle();
  co_return value * 2;
}

task<int> counted_child(int &resumes) {
  ++resumes;
  co_return 7;
}

task<void> await_child_twice(task<int> child, int &first_value, bool &caught) {
  first_value = co_await std::move(child);
  try {
    (void)co_await std::move(child);
  } catch (const UsageError &) {
    caught = true;
  }
}

class ChildError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

task<int> failing_child() {
  co_await cororeplay::yield();
  throw ChildError("child failure");
}

task<void> catching_parent(std::string &message, bool &resumed_after_catch) {
  try {
    (void)co_await failing_child();
  } catch (const ChildError &error) {
    message = error.what();
  }
  resumed_after_catch = true;
}

task<void> failing_root() {
  co_await cororeplay::yield();
  throw std::runtime_error("root failure");
}

task<void> blocked_with_probe(manual_event &event, int &live_count) {
  LifetimeProbe probe{live_count};
  (void)probe;
  co_await event.wait();
}

} // namespace

TEST_CASE("an unstarted lazy task destroys its coroutine frame without running its body",
          "[task][lifetime]") {
  int live_count = 0;
  int starts = 0;

  {
    auto lazy = lazy_with_probe(LifetimeProbe{live_count}, starts);
    REQUIRE(lazy.valid());
    CHECK(starts == 0);
    CHECK(live_count == 1);
  }

  CHECK(starts == 0);
  CHECK(live_count == 0);
}

TEST_CASE("moved-from tasks are empty and safe to destroy or reuse", "[task][move]") {
  task<int> empty;
  CHECK_FALSE(empty.valid());

  auto original = immediate_value(9);
  auto moved = std::move(original);
  CHECK_FALSE(original.valid());
  REQUIRE(moved.valid());

  task<int> assigned;
  assigned = std::move(moved);
  CHECK_FALSE(moved.valid());
  REQUIRE(assigned.valid());

  Runtime runtime;
  const auto id = runtime.spawn(std::move(assigned));
  CHECK_FALSE(assigned.valid());
  REQUIRE(runtime.run().success());
  CHECK(runtime.result<int>(id) == 9);

  Runtime empty_runtime;
  CHECK_THROWS_AS(empty_runtime.spawn(std::move(assigned)), UsageError);
}

TEST_CASE("task results support move-only non-default-constructible values", "[task][result]") {
  Runtime runtime;
  const auto id = runtime.spawn(move_only_value());

  REQUIRE(runtime.run().success());
  auto result = runtime.result<MoveOnlyResult>(id);
  CHECK(result.value == 73);
}

TEST_CASE("nested children propagate values through every parent", "[task][child][result]") {
  Runtime runtime;
  const auto root = runtime.spawn(nested_root());

  REQUIRE(runtime.run().success());
  CHECK(runtime.result<int>(root) == 84);
  CHECK(runtime.state(root) == OperationState::completed);
}

TEST_CASE("a child is resumed and consumed only once", "[task][child][single-use]") {
  int child_resumes = 0;
  int first_value = 0;
  bool caught_empty_task = false;

  Runtime runtime;
  const auto root = runtime.spawn(
      await_child_twice(counted_child(child_resumes), first_value, caught_empty_task));
  const auto &record = runtime.run();

  REQUIRE(record.success());
  CHECK(first_value == 7);
  CHECK(caught_empty_task);
  CHECK(child_resumes == 1);

  constexpr cororeplay::OperationId child = 2;
  CHECK(std::count_if(record.events.begin(), record.events.end(), [](const auto &event) {
          return event.kind == cororeplay::EventKind::operation_resume && event.operation == child;
        }) == 1);
  CHECK_THROWS_AS(runtime.result<int>(child), UsageError);
  CHECK(runtime.state(root) == OperationState::completed);
}

TEST_CASE("a parent can catch its child's exception and continue", "[task][child][exception]") {
  std::string message;
  bool resumed_after_catch = false;

  Runtime runtime;
  const auto root = runtime.spawn(catching_parent(message, resumed_after_catch));
  REQUIRE(runtime.run().success());

  CHECK(message == "child failure");
  CHECK(resumed_after_catch);
  CHECK(runtime.state(root) == OperationState::completed);
  CHECK_THROWS_AS(runtime.result<int>(2), ChildError);
}

TEST_CASE("an unhandled root exception fails the run and remains rethrowable",
          "[task][exception]") {
  Runtime runtime;
  const auto root = runtime.spawn(failing_root());
  const auto &record = runtime.run();

  CHECK(record.terminal.kind == FailureKind::unhandled_exception);
  CHECK(record.terminal.code == "unhandled_root_exception");
  CHECK(runtime.state(root) == OperationState::completed);
  CHECK_THROWS_AS(runtime.result<void>(root), std::runtime_error);
}

TEST_CASE("runtime teardown destroys a coroutine blocked on an event", "[task][lifetime][wait]") {
  int live_count = 0;

  {
    Runtime runtime;
    auto &event = runtime.make_manual_event();
    const auto root = runtime.spawn(blocked_with_probe(event, live_count));
    const auto &record = runtime.run();

    REQUIRE(record.terminal.kind == FailureKind::orphan_wait);
    CHECK(record.terminal.code == "orphan_wait");
    REQUIRE(record.waiting.size() == 1);
    CHECK(record.waiting.front().operation == root);
    CHECK(record.waiting.front().wait_kind == WaitKind::event);
    CHECK(live_count == 1);
  }

  CHECK(live_count == 0);
}
