#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include <cororeplay/artifact.hpp>
#include <cororeplay/explore.hpp>
#include <scenarios.hpp>

namespace {

using namespace cororeplay;

std::size_t occurrences(const std::string &text, const std::string &needle) {
  std::size_t count = 0;
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::string::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

Artifact timed_artifact() {
  Runtime runtime(RuntimeConfig{7, {}, SchedulingPolicy::fifo});
  const auto scenario = examples::deadlock_lock_order();
  scenario.setup(runtime);
  Artifact artifact;
  artifact.scenario_id = scenario.id;
  artifact.scenario_version = scenario.semantic_version;
  artifact.record = runtime.run();
  return artifact;
}

} // namespace

TEST_CASE("Chrome Trace export contains metadata and one instant per semantic event", "[trace]") {
  const auto artifact = timed_artifact();
  const auto trace = export_chrome_trace(artifact);
  CHECK(trace.find("\"traceEvents\"") != std::string::npos);
  CHECK(trace.find("\"displayTimeUnit\": \"ns\"") != std::string::npos);
  CHECK(trace.find("\"process_name\"") != std::string::npos);
  CHECK(trace.find("\"thread_name\"") != std::string::npos);
  CHECK(occurrences(trace, "\"ph\": \"i\"") == artifact.record.events.size());
  CHECK(trace.find("\"operation_resume\"") != std::string::npos);
  CHECK(trace.find("\"failure\"") != std::string::npos);
  CHECK(trace.find("\"ts\": 0.0") != std::string::npos);
}

TEST_CASE("Chrome Trace export is stable and contains no non-finite timestamps", "[trace]") {
  const auto artifact = timed_artifact();
  const auto first = export_chrome_trace(artifact);
  const auto second = export_chrome_trace(artifact);
  CHECK(first == second);
  CHECK(first.find("NaN") == std::string::npos);
  CHECK(first.find("Infinity") == std::string::npos);
  CHECK(first.find("typeid") == std::string::npos);
}

TEST_CASE("Chrome Trace export uses the same event buffer as replay", "[trace][replay]") {
  const auto artifact = timed_artifact();
  REQUIRE(replay(examples::deadlock_lock_order(), artifact).success);
  for (const auto &event : artifact.record.events) {
    const auto needle = "\"event_index\": " + std::to_string(event.index);
    CHECK(export_chrome_trace(artifact).find(needle) != std::string::npos);
  }
}
