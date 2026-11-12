#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

#include <cororeplay/artifact.hpp>
#include <cororeplay/explore.hpp>
#include <scenarios.hpp>

namespace {

using Clock = std::chrono::steady_clock;

template <typename Work> double milliseconds(Work work) {
  const auto start = Clock::now();
  work();
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void row(const std::string &name, double elapsed, std::uint64_t units) {
  std::cout << "case=" << name << " units=" << units << " elapsed_ms=" << std::fixed
            << std::setprecision(3) << elapsed << " units_per_second=" << std::setprecision(0)
            << static_cast<double>(units) / (elapsed / 1000.0) << '\n';
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto decisions = argc > 1 ? std::stoull(argv[1]) : 2000ULL;
    if (decisions == 0) {
      throw std::invalid_argument("decision count must be positive");
    }
#if defined(__clang__)
    std::cout << "compiler=Clang " << __clang_version__ << '\n';
#elif defined(__GNUC__)
    std::cout << "compiler=GCC " << __VERSION__ << '\n';
#endif
#if defined(NDEBUG)
    std::cout << "configuration=Release\n";
#else
    std::cout << "configuration=Debug\n";
#endif

    const auto directory = std::filesystem::temp_directory_path() / "cororeplay-trace-replay-bench";
    cororeplay::ExploreOptions options;
    options.base_seed = 7;
    options.runs = 1;
    options.output_directory = directory;
    options.limits.max_decisions = decisions;
    options.limits.max_trace_events = decisions * 4 + 16;
    const auto scenario = cororeplay::examples::no_progress();

    cororeplay::ExploreReport explored;
    const auto explore_elapsed =
        milliseconds([&] { explored = cororeplay::explore(scenario, options); });
    if (explored.failures != 1) {
      throw std::runtime_error("explore benchmark did not produce its expected artifact");
    }
    row("explore_with_artifact", explore_elapsed, decisions);

    cororeplay::Artifact artifact;
    const auto load_elapsed =
        milliseconds([&] { artifact = cororeplay::load_artifact(explored.first_artifact); });
    row("artifact_load", load_elapsed, artifact.record.events.size());

    cororeplay::ReplayReport replayed;
    const auto replay_elapsed =
        milliseconds([&] { replayed = cororeplay::replay(scenario, artifact); });
    if (!replayed.success) {
      throw std::runtime_error("replay benchmark diverged: " + replayed.divergence.field);
    }
    row("strict_replay", replay_elapsed, decisions);

    const auto save_path = directory / "benchmark-artifact.json";
    const auto save_elapsed = milliseconds([&] { cororeplay::save_artifact(artifact, save_path); });
    row("artifact_save", save_elapsed, artifact.record.events.size());

    std::size_t trace_bytes = 0;
    const auto trace_elapsed =
        milliseconds([&] { trace_bytes = cororeplay::export_chrome_trace(artifact).size(); });
    row("chrome_trace_export", trace_elapsed, artifact.record.events.size());
    std::cout << "trace_bytes=" << trace_bytes << '\n';

    const auto pair_elapsed =
        milliseconds([&] { (void)cororeplay::write_artifacts(artifact, directory / "pair"); });
    row("artifact_and_trace_write", pair_elapsed, artifact.record.events.size());
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "trace_replay_bench: " << error.what() << '\n';
    return 1;
  }
}
