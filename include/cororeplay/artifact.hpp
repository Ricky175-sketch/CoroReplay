#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <cororeplay/runtime.hpp>

namespace cororeplay {

inline constexpr std::uint32_t kArtifactSchemaVersion = 1;
inline constexpr std::uint32_t kSchedulerSemanticsVersion = 1;
inline constexpr std::string_view kRngVersion = "mt19937_64-rejection-v1";

struct Artifact {
  std::uint32_t schema_version = kArtifactSchemaVersion;
  std::uint32_t scheduler_semantics_version = kSchedulerSemanticsVersion;
  std::string scenario_id;
  std::uint32_t scenario_version = 0;
  std::uint64_t run_index = 0;
  std::uint64_t base_seed = 0;
  std::uint64_t effective_seed = 0;
  std::string rng_version = std::string(kRngVersion);
  RunLimits limits;
  RunRecord record;
};

struct ArtifactPaths {
  std::filesystem::path artifact;
  std::filesystem::path trace;
};

void save_artifact(const Artifact &artifact, const std::filesystem::path &file);
Artifact load_artifact(const std::filesystem::path &file);
ArtifactPaths write_artifacts(const Artifact &artifact, const std::filesystem::path &directory);

std::string export_chrome_trace(const Artifact &artifact);
void export_chrome_trace(const Artifact &artifact, const std::filesystem::path &file);

} // namespace cororeplay
