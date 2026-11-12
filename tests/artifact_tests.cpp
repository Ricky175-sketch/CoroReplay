#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <cororeplay/artifact.hpp>
#include <cororeplay/explore.hpp>
#include <scenarios.hpp>

namespace {

using namespace cororeplay;

Artifact sample_artifact() {
  Runtime runtime(RuntimeConfig{7191089600892374487ULL, {}, SchedulingPolicy::random});
  const auto scenario = examples::orphan_event_wait();
  scenario.setup(runtime);
  Artifact artifact;
  artifact.scenario_id = scenario.id;
  artifact.scenario_version = scenario.semantic_version;
  artifact.base_seed = 7;
  artifact.effective_seed = 7191089600892374487ULL;
  artifact.record = runtime.run();
  return artifact;
}

std::string contents(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("artifact JSON round trip preserves every semantic field", "[artifact]") {
  const auto original = sample_artifact();
  const auto directory = std::filesystem::temp_directory_path() / "cororeplay-artifact-roundtrip";
  const auto path = directory / "artifact.json";
  save_artifact(original, path);
  const auto loaded = load_artifact(path);

  CHECK(loaded.schema_version == original.schema_version);
  CHECK(loaded.scheduler_semantics_version == original.scheduler_semantics_version);
  CHECK(loaded.scenario_id == original.scenario_id);
  CHECK(loaded.scenario_version == original.scenario_version);
  CHECK(loaded.run_index == original.run_index);
  CHECK(loaded.base_seed == original.base_seed);
  CHECK(loaded.effective_seed == original.effective_seed);
  CHECK(loaded.rng_version == original.rng_version);
  CHECK(loaded.limits.max_decisions == original.limits.max_decisions);
  CHECK(loaded.limits.max_operations == original.limits.max_operations);
  CHECK(loaded.limits.max_trace_events == original.limits.max_trace_events);
  CHECK(loaded.record.decisions == original.record.decisions);
  CHECK(loaded.record.events == original.record.events);
  CHECK(loaded.record.terminal == original.record.terminal);
  CHECK(loaded.record.waiting == original.record.waiting);
}

TEST_CASE("write_artifacts creates independent artifact and trace files", "[artifact][trace]") {
  const auto directory = std::filesystem::temp_directory_path() / "cororeplay-artifact-pair";
  const auto paths = write_artifacts(sample_artifact(), directory);
  REQUIRE(std::filesystem::exists(paths.artifact));
  REQUIRE(std::filesystem::exists(paths.trace));
  CHECK(paths.artifact.filename() == "artifact.json");
  CHECK(paths.trace.filename() == "trace.json");
  CHECK(contents(paths.artifact).find("\"schema_version\": 1") != std::string::npos);
  CHECK(contents(paths.trace).find("\"traceEvents\"") != std::string::npos);
}

TEST_CASE("artifact loading rejects malformed JSON", "[artifact][negative]") {
  const auto directory = std::filesystem::temp_directory_path() / "cororeplay-artifact-malformed";
  std::filesystem::create_directories(directory);
  const auto path = directory / "bad.json";
  std::ofstream output(path);
  output << "{not-json";
  output.close();
  CHECK_THROWS(load_artifact(path));
}

TEST_CASE("failure serialization omits platform exception diagnostics", "[artifact][diagnostics]") {
  Runtime runtime(RuntimeConfig{7191089600892374487ULL, {}, SchedulingPolicy::random});
  const auto scenario = examples::exception_propagation();
  scenario.setup(runtime);
  Artifact artifact;
  artifact.scenario_id = scenario.id;
  artifact.scenario_version = scenario.semantic_version;
  artifact.record = runtime.run();
  const auto path = std::filesystem::temp_directory_path() / "cororeplay-exception-artifact.json";
  save_artifact(artifact, path);
  const auto serialized = contents(path);
  CHECK(serialized.find("unhandled_root_exception") != std::string::npos);
  CHECK(serialized.find("diagnostic text is intentionally not semantic") == std::string::npos);
  CHECK(serialized.find(__FILE__) == std::string::npos);
}

TEST_CASE("explore returns structured reports and stops at the first failure",
          "[artifact][explore]") {
  ExploreOptions options;
  options.base_seed = 7;
  options.runs = 8;
  options.output_directory = std::filesystem::temp_directory_path() / "cororeplay-explore-report";
  const auto report = explore(examples::deadlock_lock_order(), options);
  REQUIRE(report.runs_completed == 1);
  REQUIRE(report.failures == 1);
  REQUIRE(report.runs.size() == 1);
  CHECK(report.runs.front().effective_seed == 7191089600892374487ULL);
  CHECK(report.runs.front().terminal.kind == FailureKind::deadlock);
  CHECK(std::filesystem::exists(report.first_artifact));
  CHECK(replay(examples::deadlock_lock_order(), load_artifact(report.first_artifact)).success);
}
