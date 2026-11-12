#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <cororeplay/artifact.hpp>
#include <cororeplay/runtime.hpp>

namespace cororeplay {

struct Scenario {
  std::string id;
  std::uint32_t semantic_version = 1;
  std::function<void(Runtime &)> setup;
};

struct ExploreOptions {
  std::uint64_t base_seed = 0;
  std::uint64_t runs = 1;
  std::filesystem::path output_directory = "cororeplay-artifacts";
  RunLimits limits;
  bool stop_on_first_failure = true;
};

struct ExplorationRun {
  std::uint64_t run_index = 0;
  std::uint64_t effective_seed = 0;
  TerminalStatus terminal;
  std::uint64_t decisions = 0;
  std::uint64_t events = 0;
  std::filesystem::path artifact_directory;
};

struct ExploreReport {
  std::uint64_t runs_completed = 0;
  std::uint64_t failures = 0;
  std::vector<ExplorationRun> runs;
  std::filesystem::path first_artifact;
};

ExploreReport explore(const Scenario &scenario, const ExploreOptions &options = {});
ReplayReport replay(const Scenario &scenario, const Artifact &artifact);

} // namespace cororeplay
