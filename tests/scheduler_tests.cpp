#include <catch2/catch_test_macros.hpp>

#include <cororeplay/runtime.hpp>
#include <cororeplay/task.hpp>

#include <internal/random.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

using cororeplay::DecisionKind;
using cororeplay::FailureKind;
using cororeplay::OperationId;
using cororeplay::RunRecord;
using cororeplay::Runtime;
using cororeplay::RuntimeConfig;
using cororeplay::SchedulingPolicy;
using cororeplay::task;

task<void> record_steps(std::vector<int> &execution, int label, int yields) {
  execution.push_back(label * 10);
  for (int index = 0; index < yields; ++index) {
    co_await cororeplay::yield();
    execution.push_back(label * 10 + index + 1);
  }
}

task<void> yield_repeatedly() {
  for (int index = 0; index < 5; ++index) {
    co_await cororeplay::yield();
  }
}

task<void> increment_on_start(int &starts) {
  ++starts;
  co_return;
}

struct ScheduleResult {
  RunRecord record;
  std::vector<int> execution;
};

ScheduleResult run_random_schedule(std::uint64_t seed) {
  RuntimeConfig config;
  config.seed = seed;
  config.policy = SchedulingPolicy::random;

  ScheduleResult result;
  Runtime runtime(config);
  for (int label = 1; label <= 3; ++label) {
    runtime.spawn(record_steps(result.execution, label, 2));
  }
  result.record = runtime.run();
  return result;
}

} // namespace

TEST_CASE("FIFO scheduling rotates ready work and records explicit one-ready decisions",
          "[scheduler][fifo][decisions]") {
  RuntimeConfig config;
  config.policy = SchedulingPolicy::fifo;
  Runtime runtime(config);
  std::vector<int> execution;

  const auto first = runtime.spawn(record_steps(execution, 1, 1));
  const auto second = runtime.spawn(record_steps(execution, 2, 0));
  const auto &record = runtime.run();

  REQUIRE(record.success());
  CHECK(execution == std::vector<int>{10, 20, 11});
  REQUIRE(record.decisions.size() == 3);

  const std::vector<OperationId> expected_selection{first, second, first};
  for (std::size_t index = 0; index < record.decisions.size(); ++index) {
    INFO("decision = " << index);
    CHECK(record.decisions[index].kind == DecisionKind::resume);
    CHECK(record.decisions[index].selected_operation == expected_selection[index]);
  }
  CHECK(record.decisions[0].ready == std::vector<OperationId>{first, second});
  CHECK(record.decisions[1].ready == std::vector<OperationId>{first, second});
  CHECK(record.decisions[2].ready == std::vector<OperationId>{first});
}

TEST_CASE("the same seed reproduces the complete random schedule", "[scheduler][random]") {
  const auto first = run_random_schedule(0x123456789abcdef0ULL);
  const auto second = run_random_schedule(0x123456789abcdef0ULL);

  REQUIRE(first.record.success());
  REQUIRE(second.record.success());
  REQUIRE(first.record.decisions.size() == 9);
  CHECK(first.execution == second.execution);
  CHECK(first.record.decisions == second.record.decisions);
  CHECK(first.record.events == second.record.events);
  CHECK(first.record.terminal == second.record.terminal);
  CHECK(first.record.waiting == second.record.waiting);

  for (const auto &decision : first.record.decisions) {
    REQUIRE(decision.kind == DecisionKind::resume);
    CHECK(std::find(decision.ready.begin(), decision.ready.end(), decision.selected_operation) !=
          decision.ready.end());
  }
}

TEST_CASE("Random next matches mt19937_64 golden vectors", "[scheduler][rng][golden]") {
  struct Golden {
    std::uint64_t seed;
    std::array<std::uint64_t, 4> values;
  };
  constexpr std::array goldens{
      Golden{0,
             {2947667278772165694ULL, 18301848765998365067ULL, 729919693006235833ULL,
              11021831128136023278ULL}},
      Golden{1,
             {2469588189546311528ULL, 2516265689700432462ULL, 8323445853463659930ULL,
              387828560950575246ULL}},
      Golden{42,
             {13930160852258120406ULL, 11788048577503494824ULL, 13874630024467741450ULL,
              2513787319205155662ULL}},
      Golden{0x123456789abcdef0ULL,
             {4629415796178230021ULL, 9863763647169120422ULL, 570403635063972466ULL,
              16985743762141246882ULL}},
  };

  for (const auto &golden : goldens) {
    INFO("seed = " << golden.seed);
    cororeplay::detail::Random random(golden.seed);
    for (const auto expected : golden.values) {
      CHECK(random.next() == expected);
    }
  }
}

TEST_CASE("Random bounded uses rejection sampling with stable golden vectors",
          "[scheduler][rng][golden]") {
  struct Golden {
    std::uint64_t seed;
    std::uint64_t bounded;
    std::uint64_t next;
  };
  constexpr std::uint64_t bound = 0x8000000000000001ULL;
  constexpr std::array goldens{
      Golden{0, 9078476729143589258ULL, 729919693006235833ULL},
      Golden{1, 7588216632478230600ULL, 8683844110200328628ULL},
      Golden{42, 4706788815403344597ULL, 11788048577503494824ULL},
  };

  for (const auto &golden : goldens) {
    INFO("seed = " << golden.seed);
    cororeplay::detail::Random random(golden.seed);
    CHECK(random.bounded(bound) == golden.bounded);
    CHECK(random.next() == golden.next);
  }

  cororeplay::detail::Random random(0);
  CHECK_THROWS_AS(random.bounded(0), std::invalid_argument);
}

TEST_CASE("effective_seed matches independent SplitMix64 golden vectors",
          "[scheduler][rng][golden]") {
  struct Golden {
    std::uint64_t base;
    std::uint64_t run;
    std::uint64_t seed;
  };
  constexpr std::array goldens{
      Golden{0, 0, 16294208416658607535ULL},
      Golden{0, 1, 7960286522194355700ULL},
      Golden{42, 7, 14769051326987775908ULL},
      Golden{0xffffffffffffffffULL, 2, 4048727598324417001ULL},
  };

  for (const auto &golden : goldens) {
    INFO("base = " << golden.base << ", run = " << golden.run);
    CHECK(cororeplay::detail::effective_seed(golden.base, golden.run) == golden.seed);
  }
}

TEST_CASE("the decision limit stops scheduling before another resume",
          "[scheduler][limits][decisions]") {
  RuntimeConfig config;
  config.policy = SchedulingPolicy::fifo;
  config.limits.max_decisions = 2;
  Runtime runtime(config);
  runtime.spawn(yield_repeatedly());

  const auto &record = runtime.run();
  CHECK(record.terminal.kind == FailureKind::limit_exceeded);
  CHECK(record.terminal.code == "max_decisions");
  REQUIRE(record.decisions.size() == 2);
  for (const auto &decision : record.decisions) {
    CHECK(decision.kind == DecisionKind::resume);
    CHECK(decision.selected_operation == 1);
  }
}

TEST_CASE("the operation limit rejects excess roots without running earlier work",
          "[scheduler][limits][operations]") {
  RuntimeConfig config;
  config.limits.max_operations = 1;
  Runtime runtime(config);
  int starts = 0;

  const auto accepted = runtime.spawn(increment_on_start(starts));
  const auto rejected = runtime.spawn(increment_on_start(starts));
  const auto &record = runtime.run();

  CHECK(accepted == 1);
  CHECK(rejected == cororeplay::kNoOperation);
  CHECK(record.terminal.kind == FailureKind::limit_exceeded);
  CHECK(record.terminal.code == "max_operations");
  CHECK(record.decisions.empty());
  CHECK(starts == 0);
}

TEST_CASE("the trace event limit is a hard cap", "[scheduler][limits][events]") {
  RuntimeConfig config;
  config.limits.max_trace_events = 3;
  Runtime runtime(config);
  int starts = 0;
  runtime.spawn(increment_on_start(starts));

  const auto &record = runtime.run();
  CHECK(record.terminal.kind == FailureKind::limit_exceeded);
  CHECK(record.terminal.code == "max_trace_events");
  CHECK(record.events.size() == config.limits.max_trace_events);
  REQUIRE(record.decisions.size() == 1);
  CHECK(record.decisions.front().event_end == record.events.size());
  REQUIRE_FALSE(record.events.empty());
  CHECK(record.events.back().kind == cororeplay::EventKind::operation_resume);
  CHECK(starts == 1);
}

TEST_CASE("trace and schedule recording can be disabled without changing execution",
          "[scheduler][benchmark]") {
  RuntimeConfig config;
  config.policy = SchedulingPolicy::fifo;
  config.trace_enabled = false;
  config.schedule_enabled = false;
  config.limits.max_decisions = 16;
  Runtime runtime(config);
  runtime.spawn(yield_repeatedly());
  const auto &record = runtime.run();
  CHECK(record.success());
  CHECK(record.decisions.empty());
  CHECK(record.events.empty());
}
