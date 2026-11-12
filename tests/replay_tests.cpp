#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <cororeplay/artifact.hpp>
#include <cororeplay/explore.hpp>
#include <scenarios.hpp>

namespace {

using namespace cororeplay;

Artifact run_artifact(const Scenario &scenario) {
  constexpr std::uint64_t effective = 7191089600892374487ULL;
  Runtime runtime(RuntimeConfig{effective, {}, SchedulingPolicy::random});
  scenario.setup(runtime);
  Artifact artifact;
  artifact.scenario_id = scenario.id;
  artifact.scenario_version = scenario.semantic_version;
  artifact.base_seed = 7;
  artifact.effective_seed = effective;
  artifact.record = runtime.run();
  return artifact;
}

Artifact deadlock_artifact() { return run_artifact(examples::deadlock_lock_order()); }

std::string bytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void require_divergence(const Artifact &artifact, ReplayPhase phase, const std::string &field) {
  const auto report = replay(examples::deadlock_lock_order(), artifact);
  REQUIRE_FALSE(report.success);
  CHECK(report.divergence.phase == phase);
  CHECK(report.divergence.field == field);
  CHECK(report.divergence.index <= artifact.record.events.size());
}

} // namespace

TEST_CASE("strict replay accepts an identical failure artifact", "[replay]") {
  const auto artifact = deadlock_artifact();
  REQUIRE(artifact.record.terminal.kind == FailureKind::deadlock);
  const auto report = replay(examples::deadlock_lock_order(), artifact);
  REQUIRE(report.success);
  CHECK(report.terminal == artifact.record.terminal);
  CHECK(report.decisions_replayed == artifact.record.decisions.size());
  CHECK(report.events_replayed == artifact.record.events.size());
}

TEST_CASE("strict replay also accepts a successful run", "[replay]") {
  const auto artifact = run_artifact(examples::invariant_lost_update(true));
  REQUIRE(artifact.record.success());
  const auto report = replay(examples::invariant_lost_update(true), artifact);
  INFO("phase=" << static_cast<int>(report.divergence.phase) << " index=" << report.divergence.index
                << " field=" << report.divergence.field << " expected="
                << report.divergence.expected << " actual=" << report.divergence.actual);
  CHECK(report.success);
}

TEST_CASE("replay rejects a mutated selected operation even when it remains runnable",
          "[replay][mutation]") {
  auto artifact = deadlock_artifact();
  auto &decision = artifact.record.decisions.front();
  REQUIRE(decision.ready.size() == 2);
  decision.selected_operation = decision.selected_operation == decision.ready.front()
                                    ? decision.ready.back()
                                    : decision.ready.front();
  require_divergence(artifact, ReplayPhase::decision, "selected_operation");
}

TEST_CASE("replay compares the complete sorted Ready set", "[replay][mutation]") {
  auto artifact = deadlock_artifact();
  std::reverse(artifact.record.decisions.front().ready.begin(),
               artifact.record.decisions.front().ready.end());
  require_divergence(artifact, ReplayPhase::decision, "ready");
}

TEST_CASE("replay stops at the first deleted semantic wait event", "[replay][mutation]") {
  auto artifact = deadlock_artifact();
  const auto wait =
      std::find_if(artifact.record.events.begin(), artifact.record.events.end(),
                   [](const Event &event) { return event.kind == EventKind::wait_start; });
  REQUIRE(wait != artifact.record.events.end());
  const auto expected_index = wait->index;
  artifact.record.events.erase(wait);
  const auto report = replay(examples::deadlock_lock_order(), artifact);
  REQUIRE_FALSE(report.success);
  CHECK(report.divergence.phase == ReplayPhase::event);
  CHECK(report.divergence.index == expected_index);
  CHECK_FALSE(report.divergence.common_context.empty());
  CHECK_FALSE(report.divergence.operation_state.empty());
}

TEST_CASE("replay detects a changed mutex owner and reports resource state", "[replay][mutation]") {
  auto artifact = deadlock_artifact();
  const auto acquire =
      std::find_if(artifact.record.events.begin(), artifact.record.events.end(),
                   [](const Event &event) { return event.kind == EventKind::mutex_acquire; });
  REQUIRE(acquire != artifact.record.events.end());
  acquire->value += 10;
  const auto report = replay(examples::deadlock_lock_order(), artifact);
  REQUIRE_FALSE(report.success);
  CHECK(report.divergence.phase == ReplayPhase::event);
  CHECK(report.divergence.field == "value");
  CHECK(report.divergence.resource != 0);
  CHECK(report.divergence.resource_state.find("owner=") != std::string::npos);
}

TEST_CASE("replay rejects a truncated decision schedule", "[replay][mutation]") {
  auto artifact = deadlock_artifact();
  artifact.record.decisions.pop_back();
  const auto report = replay(examples::deadlock_lock_order(), artifact);
  REQUIRE_FALSE(report.success);
  CHECK((report.divergence.phase == ReplayPhase::schedule ||
         report.divergence.phase == ReplayPhase::decision));
}

TEST_CASE("replay rejects an additional decision", "[replay][mutation]") {
  auto artifact = deadlock_artifact();
  auto extra = artifact.record.decisions.back();
  ++extra.index;
  extra.event_begin = artifact.record.events.size();
  extra.event_end = extra.event_begin;
  artifact.record.decisions.push_back(std::move(extra));
  require_divergence(artifact, ReplayPhase::schedule, "decision_count");
}

TEST_CASE("replay rejects a truncated event stream", "[replay][mutation]") {
  auto artifact = deadlock_artifact();
  artifact.record.events.pop_back();
  require_divergence(artifact, ReplayPhase::event, "event_count");
}

TEST_CASE("replay rejects an additional semantic event", "[replay][mutation]") {
  auto artifact = deadlock_artifact();
  Event extra;
  extra.index = artifact.record.events.size();
  extra.kind = EventKind::invariant;
  artifact.record.events.push_back(std::move(extra));
  require_divergence(artifact, ReplayPhase::schedule, "event_count");
}

TEST_CASE("replay compares terminal status independently of the final event",
          "[replay][mutation]") {
  auto artifact = deadlock_artifact();
  artifact.record.terminal = {FailureKind::none, "success"};
  require_divergence(artifact, ReplayPhase::terminal, "terminal");
}

TEST_CASE("replay compares the final waiting snapshot", "[replay][mutation]") {
  auto artifact = deadlock_artifact();
  REQUIRE_FALSE(artifact.record.waiting.empty());
  artifact.record.waiting.front().wait_for = 99;
  require_divergence(artifact, ReplayPhase::waiting, "wait_snapshot");
}

TEST_CASE("replay validates artifact and scenario versions before setup", "[replay][metadata]") {
  auto artifact = deadlock_artifact();
  artifact.schema_version += 1;
  auto report = replay(examples::deadlock_lock_order(), artifact);
  REQUIRE_FALSE(report.success);
  CHECK(report.divergence.phase == ReplayPhase::metadata);
  CHECK(report.divergence.field == "schema_version");

  artifact = deadlock_artifact();
  artifact.scheduler_semantics_version += 1;
  CHECK(replay(examples::deadlock_lock_order(), artifact).divergence.field ==
        "scheduler_semantics_version");

  artifact = deadlock_artifact();
  artifact.scenario_version += 1;
  CHECK(replay(examples::deadlock_lock_order(), artifact).divergence.field ==
        "scenario.semantic_version");

  artifact = deadlock_artifact();
  artifact.rng_version = "unknown";
  CHECK(replay(examples::deadlock_lock_order(), artifact).divergence.field == "rng_version");
}

TEST_CASE("replay does not modify artifact bytes", "[replay][artifact]") {
  const auto directory = std::filesystem::temp_directory_path() / "cororeplay-replay-bytes";
  std::filesystem::create_directories(directory);
  const auto path = directory / "artifact.json";
  const auto artifact = deadlock_artifact();
  save_artifact(artifact, path);
  const auto before = bytes(path);
  REQUIRE(replay(examples::deadlock_lock_order(), artifact).success);
  CHECK(bytes(path) == before);
}

TEST_CASE("checked-in golden artifact replays identically one hundred times", "[replay][golden]") {
  const auto path =
      std::filesystem::path(COROREPLAY_SOURCE_DIR) / "tests/data/deadlock_artifact.json";
  const auto artifact = load_artifact(path);
  const auto before = bytes(path);
  for (int iteration = 0; iteration < 100; ++iteration) {
    const auto report = replay(examples::deadlock_lock_order(), artifact);
    REQUIRE(report.success);
    CHECK(report.decisions_replayed == 4);
    CHECK(report.events_replayed == 19);
  }
  CHECK(bytes(path) == before);
}
