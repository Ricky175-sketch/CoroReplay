#pragma once

#include <string>

#include <cororeplay/runtime.hpp>

namespace cororeplay::detail {

std::string describe_event(const Event &event);
std::string describe_terminal(const TerminalStatus &terminal);
std::string describe_wait(const WaitSnapshot &wait);
std::string decision_summary(const Decision &decision);

} // namespace cororeplay::detail
