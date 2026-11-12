#pragma once

#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>

namespace cororeplay::detail {

class Random {
public:
  explicit Random(std::uint64_t seed) : engine_(seed) {}

  std::uint64_t next() { return engine_(); }

  std::uint64_t bounded(std::uint64_t bound) {
    if (bound == 0) {
      throw std::invalid_argument("random bound must be positive");
    }
    const auto threshold = static_cast<std::uint64_t>(-bound) % bound;
    for (;;) {
      const auto value = next();
      if (value >= threshold) {
        return value % bound;
      }
    }
  }

private:
  std::mt19937_64 engine_;
};

inline std::uint64_t effective_seed(std::uint64_t base, std::uint64_t run_index) {
  auto value = base + 0x9e3779b97f4a7c15ULL * (run_index + 1);
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

} // namespace cororeplay::detail
