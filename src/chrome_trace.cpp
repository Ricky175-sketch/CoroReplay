#include <cororeplay/artifact.hpp>

#include <fstream>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace cororeplay {

std::string export_chrome_trace(const Artifact &artifact) {
  nlohmann::json events = nlohmann::json::array();
  events.push_back({{"name", "process_name"},
                    {"ph", "M"},
                    {"pid", 1},
                    {"tid", 0},
                    {"args", {{"name", "CoroReplay " + artifact.scenario_id}}}});

  std::set<OperationId> operations;
  for (const auto &event : artifact.record.events) {
    if (event.operation != kNoOperation) {
      operations.insert(event.operation);
    }
  }
  for (const auto operation : operations) {
    events.push_back({{"name", "thread_name"},
                      {"ph", "M"},
                      {"pid", 1},
                      {"tid", operation},
                      {"args", {{"name", "Operation " + std::to_string(operation)}}}});
  }

  for (const auto &event : artifact.record.events) {
    nlohmann::json args = {
        {"event_index", event.index},   {"decision_index", event.decision_index},
        {"operation", event.operation}, {"related_operation", event.related_operation},
        {"resource", event.resource},   {"timer", event.timer},
        {"value", event.value},         {"value2", event.value2}};
    if (!event.text.empty()) {
      args["text"] = event.text;
    }
    events.push_back({{"name", to_string(event.kind)},
                      {"cat", "cororeplay"},
                      {"ph", "i"},
                      {"s", "t"},
                      {"ts", static_cast<double>(event.time.nanoseconds) / 1000.0},
                      {"pid", 1},
                      {"tid", event.operation},
                      {"args", std::move(args)}});
  }

  nlohmann::json trace = {{"traceEvents", std::move(events)}, {"displayTimeUnit", "ns"}};
  return trace.dump(2) + '\n';
}

void export_chrome_trace(const Artifact &artifact, const std::filesystem::path &file) {
  if (!file.parent_path().empty()) {
    std::filesystem::create_directories(file.parent_path());
  }
  std::ofstream output(file, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot open Chrome Trace for writing: " + file.string());
  }
  output << export_chrome_trace(artifact);
  if (!output) {
    throw std::runtime_error("cannot write Chrome Trace: " + file.string());
  }
}

} // namespace cororeplay
