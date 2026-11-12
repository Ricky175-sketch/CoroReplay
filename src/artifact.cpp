#include <cororeplay/artifact.hpp>

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace cororeplay {
namespace {

using Json = nlohmann::json;

template <typename T, typename Encode>
Json encode_array(const std::vector<T> &values, Encode encode) {
  auto json = Json::array();
  for (const auto &value : values) {
    json.push_back(encode(value));
  }
  return json;
}

template <typename T, typename Decode>
std::vector<T> decode_array(const Json &json, Decode decode) {
  std::vector<T> values;
  values.reserve(json.size());
  for (const auto &value : json) {
    values.push_back(decode(value));
  }
  return values;
}

template <typename Enum, std::size_t N> Enum parse_enum(const Json &json, const Enum (&values)[N]) {
  const auto text = json.get<std::string>();
  for (const auto value : values) {
    if (text == to_string(value)) {
      return value;
    }
  }
  throw std::runtime_error("unknown enum value in artifact: " + text);
}

Json to_json(const VirtualTime &time) { return time.nanoseconds; }

VirtualTime time_from_json(const Json &json) { return VirtualTime{json.get<std::uint64_t>()}; }

Json to_json(const TimerWake &wake) {
  return {{"timer", wake.timer}, {"operation", wake.operation}};
}

TimerWake wake_from_json(const Json &json) {
  return {json.at("timer").get<TimerId>(), json.at("operation").get<OperationId>()};
}

Json to_json(const Decision &decision) {
  return {
      {"index", decision.index},
      {"kind", to_string(decision.kind)},
      {"time_ns", to_json(decision.time)},
      {"end_time_ns", to_json(decision.end_time)},
      {"selected_operation", decision.selected_operation},
      {"ready", decision.ready},
      {"event_begin", decision.event_begin},
      {"event_end", decision.event_end},
      {"wakes", encode_array(decision.wakes, [](const TimerWake &wake) { return to_json(wake); })}};
}

Decision decision_from_json(const Json &json) {
  constexpr DecisionKind kinds[] = {DecisionKind::resume, DecisionKind::advance_clock};
  Decision decision;
  decision.index = json.at("index").get<std::uint64_t>();
  decision.kind = parse_enum(json.at("kind"), kinds);
  decision.time = time_from_json(json.at("time_ns"));
  decision.end_time = time_from_json(json.at("end_time_ns"));
  decision.selected_operation = json.at("selected_operation").get<OperationId>();
  decision.ready = json.at("ready").get<std::vector<OperationId>>();
  decision.event_begin = json.at("event_begin").get<std::uint64_t>();
  decision.event_end = json.at("event_end").get<std::uint64_t>();
  decision.wakes = decode_array<TimerWake>(json.at("wakes"), wake_from_json);
  return decision;
}

Json to_json(const Event &event) {
  return {{"index", event.index},
          {"decision_index", event.decision_index},
          {"time_ns", to_json(event.time)},
          {"kind", to_string(event.kind)},
          {"operation", event.operation},
          {"related_operation", event.related_operation},
          {"resource", event.resource},
          {"timer", event.timer},
          {"value", event.value},
          {"value2", event.value2},
          {"text", event.text}};
}

Event event_from_json(const Json &json) {
  constexpr EventKind kinds[] = {
      EventKind::operation_create,   EventKind::operation_resume, EventKind::operation_suspend,
      EventKind::operation_complete, EventKind::child_attach,     EventKind::ready,
      EventKind::wait_start,         EventKind::wait_finish,      EventKind::mutex_acquire,
      EventKind::mutex_handoff,      EventKind::mutex_release,    EventKind::event_set,
      EventKind::event_reset,        EventKind::timer_arm,        EventKind::timer_fire,
      EventKind::clock_advance,      EventKind::invariant,        EventKind::failure};
  Event event;
  event.index = json.at("index").get<std::uint64_t>();
  event.decision_index = json.at("decision_index").get<std::uint64_t>();
  event.time = time_from_json(json.at("time_ns"));
  event.kind = parse_enum(json.at("kind"), kinds);
  event.operation = json.at("operation").get<OperationId>();
  event.related_operation = json.at("related_operation").get<OperationId>();
  event.resource = json.at("resource").get<ResourceId>();
  event.timer = json.at("timer").get<TimerId>();
  event.value = json.at("value").get<std::int64_t>();
  event.value2 = json.at("value2").get<std::int64_t>();
  event.text = json.at("text").get<std::string>();
  return event;
}

Json to_json(const TerminalStatus &terminal) {
  return {{"kind", to_string(terminal.kind)}, {"code", terminal.code}};
}

TerminalStatus terminal_from_json(const Json &json) {
  constexpr FailureKind kinds[] = {FailureKind::none,
                                   FailureKind::invariant_violation,
                                   FailureKind::deadlock,
                                   FailureKind::orphan_wait,
                                   FailureKind::limit_exceeded,
                                   FailureKind::unhandled_exception,
                                   FailureKind::usage_error,
                                   FailureKind::virtual_time_overflow,
                                   FailureKind::replay_divergence};
  return {parse_enum(json.at("kind"), kinds), json.at("code").get<std::string>()};
}

Json to_json(const WaitSnapshot &wait) {
  return {{"operation", wait.operation},
          {"wait_kind", to_string(wait.wait_kind)},
          {"wait_for", wait.wait_for},
          {"resource", wait.resource},
          {"timer", wait.timer}};
}

WaitSnapshot wait_from_json(const Json &json) {
  constexpr WaitKind kinds[] = {WaitKind::none, WaitKind::child, WaitKind::mutex, WaitKind::event,
                                WaitKind::timer};
  return {json.at("operation").get<OperationId>(), parse_enum(json.at("wait_kind"), kinds),
          json.at("wait_for").get<OperationId>(), json.at("resource").get<ResourceId>(),
          json.at("timer").get<TimerId>()};
}

Json to_json(const RunLimits &limits) {
  return {{"max_decisions", limits.max_decisions},
          {"max_operations", limits.max_operations},
          {"max_trace_events", limits.max_trace_events}};
}

RunLimits limits_from_json(const Json &json) {
  return {json.at("max_decisions").get<std::uint64_t>(),
          json.at("max_operations").get<std::uint64_t>(),
          json.at("max_trace_events").get<std::uint64_t>()};
}

Json to_json(const Artifact &artifact) {
  return {
      {"schema_version", artifact.schema_version},
      {"scheduler_semantics_version", artifact.scheduler_semantics_version},
      {"scenario", {{"id", artifact.scenario_id}, {"semantic_version", artifact.scenario_version}}},
      {"seed",
       {{"run_index", artifact.run_index},
        {"base", artifact.base_seed},
        {"effective", artifact.effective_seed}}},
      {"rng_version", artifact.rng_version},
      {"limits", to_json(artifact.limits)},
      {"decisions", encode_array(artifact.record.decisions,
                                 [](const Decision &value) { return to_json(value); })},
      {"events",
       encode_array(artifact.record.events, [](const Event &value) { return to_json(value); })},
      {"terminal", to_json(artifact.record.terminal)},
      {"waiting", encode_array(artifact.record.waiting,
                               [](const WaitSnapshot &value) { return to_json(value); })}};
}

Artifact artifact_from_json(const Json &json) {
  Artifact artifact;
  artifact.schema_version = json.at("schema_version").get<std::uint32_t>();
  artifact.scheduler_semantics_version =
      json.at("scheduler_semantics_version").get<std::uint32_t>();
  artifact.scenario_id = json.at("scenario").at("id").get<std::string>();
  artifact.scenario_version = json.at("scenario").at("semantic_version").get<std::uint32_t>();
  artifact.run_index = json.at("seed").at("run_index").get<std::uint64_t>();
  artifact.base_seed = json.at("seed").at("base").get<std::uint64_t>();
  artifact.effective_seed = json.at("seed").at("effective").get<std::uint64_t>();
  artifact.rng_version = json.at("rng_version").get<std::string>();
  artifact.limits = limits_from_json(json.at("limits"));
  artifact.record.decisions = decode_array<Decision>(json.at("decisions"), decision_from_json);
  artifact.record.events = decode_array<Event>(json.at("events"), event_from_json);
  artifact.record.terminal = terminal_from_json(json.at("terminal"));
  artifact.record.waiting = decode_array<WaitSnapshot>(json.at("waiting"), wait_from_json);
  return artifact;
}

} // namespace

void save_artifact(const Artifact &artifact, const std::filesystem::path &file) {
  if (!file.parent_path().empty()) {
    std::filesystem::create_directories(file.parent_path());
  }
  std::ofstream output(file, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot open artifact for writing: " + file.string());
  }
  output << to_json(artifact).dump(2) << '\n';
  if (!output) {
    throw std::runtime_error("cannot write artifact: " + file.string());
  }
}

Artifact load_artifact(const std::filesystem::path &file) {
  std::ifstream input(file, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open artifact: " + file.string());
  }
  return artifact_from_json(Json::parse(input));
}

ArtifactPaths write_artifacts(const Artifact &artifact, const std::filesystem::path &directory) {
  std::filesystem::create_directories(directory);
  ArtifactPaths paths{directory / "artifact.json", directory / "trace.json"};
  save_artifact(artifact, paths.artifact);
  export_chrome_trace(artifact, paths.trace);
  return paths;
}

} // namespace cororeplay
