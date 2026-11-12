#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include <cororeplay/artifact.hpp>
#include <cororeplay/explore.hpp>
#include <scenarios.hpp>

namespace {

using cororeplay::Scenario;

Scenario scenario_for(std::string_view name) {
  using namespace cororeplay::examples;
  if (name == "deadlock_lock_order") {
    return deadlock_lock_order();
  }
  if (name == "invariant_lost_update") {
    return invariant_lost_update();
  }
  if (name == "invariant_lost_update_fixed") {
    return invariant_lost_update(true);
  }
  if (name == "orphan_event_wait") {
    return orphan_event_wait();
  }
  if (name == "no_progress") {
    return no_progress();
  }
  if (name == "exception_propagation") {
    return exception_propagation();
  }
  if (name == "exception_propagation_caught") {
    return exception_propagation(true);
  }
  throw cororeplay::UsageError("unknown scenario: " + std::string(name));
}

std::uint64_t number(std::string_view text) {
  std::uint64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    throw cororeplay::UsageError("invalid unsigned integer: " + std::string(text));
  }
  return value;
}

std::string_view phase_name(cororeplay::ReplayPhase phase) {
  using cororeplay::ReplayPhase;
  switch (phase) {
  case ReplayPhase::none:
    return "none";
  case ReplayPhase::metadata:
    return "metadata";
  case ReplayPhase::decision:
    return "decision";
  case ReplayPhase::event:
    return "event";
  case ReplayPhase::schedule:
    return "schedule";
  case ReplayPhase::terminal:
    return "terminal";
  case ReplayPhase::waiting:
    return "waiting";
  }
  return "unknown";
}

void usage() {
  std::cout << "CoroReplay v0.1\n"
               "  cororeplay_demo explore <case> --seed <n> --runs <n> --output <dir> "
               "[--max-decisions <n>]\n"
               "  cororeplay_demo replay <case> --artifact <path>\n\n"
               "Cases: deadlock_lock_order, invariant_lost_update, "
               "invariant_lost_update_fixed, orphan_event_wait, no_progress, "
               "exception_propagation, exception_propagation_caught\n";
}

int explore_command(int argc, char **argv) {
  if (argc < 3) {
    throw cororeplay::UsageError("explore requires a scenario");
  }
  const auto scenario = scenario_for(argv[2]);
  cororeplay::ExploreOptions options;
  for (int index = 3; index < argc; index += 2) {
    if (index + 1 >= argc) {
      throw cororeplay::UsageError("missing option value");
    }
    const std::string_view option = argv[index];
    const std::string_view value = argv[index + 1];
    if (option == "--seed") {
      options.base_seed = number(value);
    } else if (option == "--runs") {
      options.runs = number(value);
    } else if (option == "--output") {
      options.output_directory = value;
    } else if (option == "--max-decisions") {
      options.limits.max_decisions = number(value);
    } else if (option == "--max-operations") {
      options.limits.max_operations = number(value);
    } else if (option == "--max-trace-events") {
      options.limits.max_trace_events = number(value);
    } else {
      throw cororeplay::UsageError("unknown explore option: " + std::string(option));
    }
  }

  const auto report = cororeplay::explore(scenario, options);
  for (const auto &run : report.runs) {
    std::cout << "run=" << run.run_index << " effective_seed=" << run.effective_seed
              << " terminal=" << cororeplay::to_string(run.terminal.kind)
              << " code=" << run.terminal.code << " decisions=" << run.decisions
              << " events=" << run.events;
    if (!run.artifact_directory.empty()) {
      std::cout << " artifact=" << (run.artifact_directory / "artifact.json");
    }
    std::cout << '\n';
  }
  return report.failures == 0 ? 0 : 1;
}

int replay_command(int argc, char **argv) {
  if (argc != 5 || std::string_view(argv[3]) != "--artifact") {
    throw cororeplay::UsageError("replay requires <case> --artifact <path>");
  }
  const auto scenario = scenario_for(argv[2]);
  const auto artifact = cororeplay::load_artifact(argv[4]);
  const auto report = cororeplay::replay(scenario, artifact);
  if (report.success) {
    std::cout << "replay success decisions=" << report.decisions_replayed
              << " events=" << report.events_replayed
              << " terminal=" << cororeplay::to_string(report.terminal.kind) << '\n';
    return 0;
  }
  const auto &divergence = report.divergence;
  std::cout << "replay divergence phase=" << phase_name(divergence.phase)
            << " index=" << divergence.index << " field=" << divergence.field << " expected=\""
            << divergence.expected << "\" actual=\"" << divergence.actual << "\" operation_state=\""
            << divergence.operation_state << "\" resource_state=\"" << divergence.resource_state
            << "\" common_context=" << divergence.common_context.size() << '\n';
  return 1;
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 2 || std::string_view(argv[1]) == "--help") {
      usage();
      return argc < 2 ? 2 : 0;
    }
    const std::string_view command = argv[1];
    if (command == "explore") {
      return explore_command(argc, argv);
    }
    if (command == "replay") {
      return replay_command(argc, argv);
    }
    throw cororeplay::UsageError("unknown command: " + std::string(command));
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }
}
