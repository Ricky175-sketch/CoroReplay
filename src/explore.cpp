#include <cororeplay/explore.hpp>

#include <cctype>
#include <stdexcept>

#include <internal/random.hpp>

namespace cororeplay {
namespace {

std::string safe_id(std::string id) {
  for (auto &character : id) {
    const auto value = static_cast<unsigned char>(character);
    if (std::isalnum(value) == 0 && character != '-' && character != '_') {
      character = '_';
    }
  }
  return id.empty() ? "scenario" : id;
}

ReplayReport metadata_divergence(std::string field, std::string expected, std::string actual) {
  ReplayReport report;
  report.divergence.phase = ReplayPhase::metadata;
  report.divergence.field = std::move(field);
  report.divergence.expected = std::move(expected);
  report.divergence.actual = std::move(actual);
  return report;
}

} // namespace

ExploreReport explore(const Scenario &scenario, const ExploreOptions &options) {
  if (scenario.id.empty() || !scenario.setup) {
    throw UsageError("Scenario requires an ID and setup factory");
  }
  if (options.runs == 0) {
    throw UsageError("explore requires at least one run");
  }

  ExploreReport report;
  for (std::uint64_t run_index = 0; run_index < options.runs; ++run_index) {
    const auto seed = detail::effective_seed(options.base_seed, run_index);
    Runtime runtime(RuntimeConfig{seed, options.limits, SchedulingPolicy::random});
    scenario.setup(runtime);
    auto record = runtime.run();

    ExplorationRun summary;
    summary.run_index = run_index;
    summary.effective_seed = seed;
    summary.terminal = record.terminal;
    summary.decisions = record.decisions.size();
    summary.events = record.events.size();
    ++report.runs_completed;

    if (!record.success()) {
      ++report.failures;
      Artifact artifact;
      artifact.scenario_id = scenario.id;
      artifact.scenario_version = scenario.semantic_version;
      artifact.run_index = run_index;
      artifact.base_seed = options.base_seed;
      artifact.effective_seed = seed;
      artifact.limits = options.limits;
      artifact.record = std::move(record);
      const auto directory =
          options.output_directory / (safe_id(scenario.id) + "-run-" + std::to_string(run_index) +
                                      "-seed-" + std::to_string(seed));
      const auto paths = write_artifacts(artifact, directory);
      summary.artifact_directory = directory;
      if (report.first_artifact.empty()) {
        report.first_artifact = paths.artifact;
      }
    }
    report.runs.push_back(std::move(summary));
    if (report.failures != 0 && options.stop_on_first_failure) {
      break;
    }
  }
  return report;
}

ReplayReport replay(const Scenario &scenario, const Artifact &artifact) {
  if (artifact.schema_version != kArtifactSchemaVersion) {
    return metadata_divergence("schema_version", std::to_string(kArtifactSchemaVersion),
                               std::to_string(artifact.schema_version));
  }
  if (artifact.scheduler_semantics_version != kSchedulerSemanticsVersion) {
    return metadata_divergence("scheduler_semantics_version",
                               std::to_string(kSchedulerSemanticsVersion),
                               std::to_string(artifact.scheduler_semantics_version));
  }
  if (artifact.scenario_id != scenario.id) {
    return metadata_divergence("scenario.id", artifact.scenario_id, scenario.id);
  }
  if (artifact.scenario_version != scenario.semantic_version) {
    return metadata_divergence("scenario.semantic_version",
                               std::to_string(artifact.scenario_version),
                               std::to_string(scenario.semantic_version));
  }
  if (artifact.rng_version != kRngVersion) {
    return metadata_divergence("rng_version", artifact.rng_version, std::string(kRngVersion));
  }

  Runtime runtime(RuntimeConfig{artifact.effective_seed, artifact.limits, SchedulingPolicy::fifo});
  ReplayReport report;
  runtime.configure_replay(artifact, report.divergence);
  scenario.setup(runtime);
  const auto &record = runtime.run();
  report.success = report.divergence.phase == ReplayPhase::none;
  report.terminal = record.terminal;
  report.decisions_replayed = record.decisions.size();
  report.events_replayed = record.events.size();
  return report;
}

} // namespace cororeplay
