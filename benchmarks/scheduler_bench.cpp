#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

#include <cororeplay/async_mutex.hpp>
#include <cororeplay/runtime.hpp>

namespace {

using Clock = std::chrono::steady_clock;

cororeplay::task<void> yield_work(std::uint64_t yields) {
  for (std::uint64_t index = 0; index < yields; ++index) {
    co_await cororeplay::yield();
  }
}

cororeplay::task<void> mutex_work(cororeplay::async_mutex &mutex, std::uint64_t iterations,
                                  std::uint64_t &value) {
  for (std::uint64_t index = 0; index < iterations; ++index) {
    co_await mutex.lock();
    ++value;
    mutex.unlock();
    co_await cororeplay::yield();
  }
}

void print_environment() {
#if defined(__clang__)
  std::cout << "compiler=Clang " << __clang_version__ << '\n';
#elif defined(__GNUC__)
  std::cout << "compiler=GCC " << __VERSION__ << '\n';
#else
  std::cout << "compiler=unknown\n";
#endif
#if defined(NDEBUG)
  std::cout << "configuration=Release\n";
#else
  std::cout << "configuration=Debug\n";
#endif
}

template <typename Work> double milliseconds(Work work) {
  const auto start = Clock::now();
  work();
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void yield_case(std::uint64_t iterations, bool trace, bool schedule) {
  cororeplay::RunLimits limits;
  limits.max_decisions = iterations + 1;
  limits.max_trace_events = iterations * 4 + 16;
  cororeplay::RuntimeConfig config{7, limits, cororeplay::SchedulingPolicy::fifo, trace, schedule};
  double elapsed = 0;
  cororeplay::TerminalStatus terminal;
  elapsed = milliseconds([&] {
    cororeplay::Runtime runtime(config);
    runtime.spawn(yield_work(iterations));
    terminal = runtime.run().terminal;
  });
  if (terminal.kind != cororeplay::FailureKind::none) {
    throw std::runtime_error("yield benchmark did not complete: " + terminal.code);
  }
  const auto rate = static_cast<double>(iterations) / (elapsed / 1000.0);
  std::cout << "case=yield iterations=" << iterations << " trace=" << trace
            << " schedule=" << schedule << " elapsed_ms=" << std::fixed << std::setprecision(3)
            << elapsed << " yields_per_second=" << std::setprecision(0) << rate << '\n';
}

void mutex_case(std::uint64_t workers, std::uint64_t iterations) {
  cororeplay::RunLimits limits;
  limits.max_decisions = workers * iterations * 4 + 100;
  cororeplay::RuntimeConfig config{7, limits, cororeplay::SchedulingPolicy::fifo, false, false};
  std::uint64_t value = 0;
  const auto elapsed = milliseconds([&] {
    cororeplay::Runtime runtime(config);
    auto &mutex = runtime.make_mutex();
    for (std::uint64_t worker = 0; worker < workers; ++worker) {
      runtime.spawn(mutex_work(mutex, iterations, value));
    }
    const auto &record = runtime.run();
    if (!record.success()) {
      throw std::runtime_error("mutex benchmark did not complete: " + record.terminal.code);
    }
  });
  const auto acquisitions = workers * iterations;
  if (value != acquisitions) {
    throw std::runtime_error("mutex benchmark produced an incorrect value");
  }
  const auto rate = static_cast<double>(acquisitions) / (elapsed / 1000.0);
  std::cout << "case=mutex_contention workers=" << workers
            << " iterations_per_worker=" << iterations
            << " trace=0 schedule=0 elapsed_ms=" << std::fixed << std::setprecision(3) << elapsed
            << " acquisitions_per_second=" << std::setprecision(0) << rate << '\n';
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto iterations = argc > 1 ? std::stoull(argv[1]) : 20000ULL;
    if (iterations == 0) {
      throw std::invalid_argument("iteration count must be positive");
    }
    print_environment();
    yield_case(iterations, false, false);
    yield_case(iterations, true, false);
    yield_case(iterations, true, true);
    mutex_case(4, iterations / 4 + 1);
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "scheduler_bench: " << error.what() << '\n';
    return 1;
  }
}
