#include <catch2/catch_test_macros.hpp>

#include <cororeplay/runtime.hpp>
#include <cororeplay/task.hpp>
#include <cororeplay/time.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using cororeplay::DecisionKind;
using cororeplay::FailureKind;
using cororeplay::OperationId;
using cororeplay::Runtime;
using cororeplay::RuntimeConfig;
using cororeplay::SchedulingPolicy;
using cororeplay::task;
using cororeplay::TimerWake;
using cororeplay::VirtualTime;
using cororeplay::WaitKind;

task<void> sleep_and_sample(Runtime &runtime, std::chrono::nanoseconds duration,
                            std::vector<std::uint64_t> &samples) {
  samples.push_back(runtime.now().nanoseconds);
  co_await cororeplay::sleep_for(duration);
  samples.push_back(runtime.now().nanoseconds);
}

task<void> yield_and_sample(Runtime &runtime, std::vector<std::uint64_t> &samples) {
  samples.push_back(runtime.now().nanoseconds);
  co_await cororeplay::yield();
  samples.push_back(runtime.now().nanoseconds);
}

task<void> overflow_after_maximum_time(Runtime &runtime, std::vector<std::uint64_t> &samples) {
  co_await cororeplay::sleep_until(VirtualTime{std::numeric_limits<std::uint64_t>::max()});
  samples.push_back(runtime.now().nanoseconds);
  co_await cororeplay::sleep_for(std::chrono::nanoseconds{1});
  samples.push_back(runtime.now().nanoseconds);
}

RuntimeConfig fifo_config() {
  RuntimeConfig config;
  config.policy = SchedulingPolicy::fifo;
  return config;
}

} // namespace

TEST_CASE("virtual time advances only after the ready set becomes empty",
          "[time][scheduler][decisions]") {
  Runtime runtime(fifo_config());
  std::vector<std::uint64_t> sleeper_samples;
  std::vector<std::uint64_t> yielder_samples;

  const auto sleeper =
      runtime.spawn(sleep_and_sample(runtime, std::chrono::nanoseconds{10}, sleeper_samples));
  const auto yielder = runtime.spawn(yield_and_sample(runtime, yielder_samples));
  const auto &record = runtime.run();

  REQUIRE(record.success());
  CHECK(sleeper_samples == std::vector<std::uint64_t>{0, 10});
  CHECK(yielder_samples == std::vector<std::uint64_t>{0, 0});
  REQUIRE(record.decisions.size() == 5);

  constexpr std::array kinds{DecisionKind::resume, DecisionKind::resume, DecisionKind::resume,
                             DecisionKind::advance_clock, DecisionKind::resume};
  const std::array selected{sleeper, yielder, yielder, cororeplay::kNoOperation, sleeper};
  for (std::size_t index = 0; index < record.decisions.size(); ++index) {
    INFO("decision = " << index);
    CHECK(record.decisions[index].kind == kinds[index]);
    CHECK(record.decisions[index].selected_operation == selected[index]);
  }
  for (std::size_t index = 0; index < 3; ++index) {
    CHECK(record.decisions[index].time.nanoseconds == 0);
    CHECK(record.decisions[index].end_time.nanoseconds == 0);
  }

  const auto &advance = record.decisions[3];
  CHECK(advance.ready.empty());
  CHECK(advance.time.nanoseconds == 0);
  CHECK(advance.end_time.nanoseconds == 10);
  CHECK(advance.wakes == std::vector<TimerWake>{{1, sleeper}});
  CHECK(record.decisions[4].ready == std::vector<OperationId>{sleeper});
}

TEST_CASE("timers with the same deadline wake in timer ID order", "[time][timer][order]") {
  Runtime runtime(fifo_config());
  std::vector<std::uint64_t> first_samples;
  std::vector<std::uint64_t> second_samples;

  const auto first =
      runtime.spawn(sleep_and_sample(runtime, std::chrono::nanoseconds{5}, first_samples));
  const auto second =
      runtime.spawn(sleep_and_sample(runtime, std::chrono::nanoseconds{5}, second_samples));
  const auto &record = runtime.run();

  REQUIRE(record.success());
  CHECK(first_samples == std::vector<std::uint64_t>{0, 5});
  CHECK(second_samples == std::vector<std::uint64_t>{0, 5});
  REQUIRE(record.decisions.size() == 5);

  const auto &advance = record.decisions[2];
  CHECK(advance.kind == DecisionKind::advance_clock);
  CHECK(advance.wakes == std::vector<TimerWake>{{1, first}, {2, second}});

  std::vector<cororeplay::TimerId> fired;
  for (const auto &event : record.events) {
    if (event.kind == cororeplay::EventKind::timer_fire) {
      CHECK(event.time.nanoseconds == 5);
      fired.push_back(event.timer);
    }
  }
  CHECK(fired == std::vector<cororeplay::TimerId>{1, 2});
}

TEST_CASE("zero sleep suspends and advances a zero-length clock decision", "[time][timer][zero]") {
  Runtime runtime(fifo_config());
  std::vector<std::uint64_t> samples;
  const auto root = runtime.spawn(sleep_and_sample(runtime, std::chrono::nanoseconds{0}, samples));
  const auto &record = runtime.run();

  REQUIRE(record.success());
  CHECK(samples == std::vector<std::uint64_t>{0, 0});
  CHECK(runtime.now().nanoseconds == 0);
  REQUIRE(record.decisions.size() == 3);
  CHECK(record.decisions[0].kind == DecisionKind::resume);
  CHECK(record.decisions[1].kind == DecisionKind::advance_clock);
  CHECK(record.decisions[2].kind == DecisionKind::resume);
  CHECK(record.decisions[1].time.nanoseconds == 0);
  CHECK(record.decisions[1].end_time.nanoseconds == 0);
  CHECK(record.decisions[1].wakes == std::vector<TimerWake>{{1, root}});
}

TEST_CASE("negative sleep durations are rejected", "[time][sleep][validation]") {
  CHECK_THROWS_AS(cororeplay::sleep_for(std::chrono::nanoseconds{-1}), cororeplay::UsageError);
}

TEST_CASE("virtual time overflow stops the run without wrapping the clock",
          "[time][timer][overflow]") {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  Runtime runtime(fifo_config());
  std::vector<std::uint64_t> samples;
  const auto root = runtime.spawn(overflow_after_maximum_time(runtime, samples));
  const auto &record = runtime.run();

  CHECK(record.terminal.kind == FailureKind::virtual_time_overflow);
  CHECK(record.terminal.code == "virtual_time_overflow");
  CHECK(runtime.now().nanoseconds == maximum);
  CHECK(samples == std::vector<std::uint64_t>{maximum});
  REQUIRE(record.decisions.size() == 3);
  CHECK(record.decisions[1].kind == DecisionKind::advance_clock);
  CHECK(record.decisions[1].end_time.nanoseconds == maximum);
  REQUIRE(record.waiting.size() == 1);
  CHECK(record.waiting.front().operation == root);
  CHECK(record.waiting.front().wait_kind == WaitKind::timer);
  CHECK(record.waiting.front().timer == 0);
}

TEST_CASE("timers still advance when trace and schedule recording are disabled",
          "[time][benchmark]") {
  auto config = fifo_config();
  config.trace_enabled = false;
  config.schedule_enabled = false;
  Runtime runtime(config);
  std::vector<std::uint64_t> samples;
  runtime.spawn(sleep_and_sample(runtime, std::chrono::nanoseconds{5}, samples));
  const auto &record = runtime.run();
  CHECK(record.success());
  CHECK(samples == std::vector<std::uint64_t>{0, 5});
  CHECK(runtime.now().nanoseconds == 5);
  CHECK(record.decisions.empty());
  CHECK(record.events.empty());
}
