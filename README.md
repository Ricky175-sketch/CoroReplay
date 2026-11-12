# CoroReplay v0.1

CoroReplay is a deterministic concurrency testing, failure reproduction, and execution visualization toolkit for C++20 coroutines. It provides a closed-world `task<T>`, a seeded single-thread scheduler, virtual time, synchronization primitives, versioned failure artifacts, strict replay, first-divergence reports, and Chrome Trace/Perfetto JSON export.

The complete workflow is:

```text
scenario -> seeded exploration -> failure artifact -> strict replay
         -> first-divergence report -> Chrome Trace/Perfetto JSON
```

CoroReplay is not a transparent adapter for arbitrary third-party coroutine libraries. It finds logical errors in interleavings of code written against its API; it does not detect real data races or emulate operating-system threads.

## Requirements and dependencies

- Linux or macOS
- CMake 3.24+
- A C++20 compiler (GCC, Clang, or AppleClang)
- Network access on the first configure, unless the FetchContent sources are already cached

Public CoroReplay headers depend only on the C++ standard library. Private artifact code uses nlohmann-json 3.11.3; tests use Catch2 3.7.1. Both are pinned to release archives with SHA-256 hashes. Benchmarks use only the standard library. The project is MIT licensed.

## Quick start

From the `CoroReplay` directory:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCOROREPLAY_BUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Useful options:

```text
COROREPLAY_BUILD_EXAMPLES=ON|OFF       # default ON
COROREPLAY_BUILD_TESTING=ON|OFF        # default OFF
COROREPLAY_BUILD_BENCHMARKS=ON|OFF     # default OFF
COROREPLAY_WARNINGS_AS_ERRORS=ON|OFF   # default OFF
COROREPLAY_ENABLE_SANITIZERS=ON|OFF    # GCC/Clang ASan+UBSan
```

To build and run the sanitizer configuration:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCOROREPLAY_BUILD_TESTING=ON -DCOROREPLAY_ENABLE_SANITIZERS=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure
```

## Explore, replay, and visualize

The demos use a regression-tested base seed (`7`). Its first effective seed is `7191089600892374487`.

Explore the lock-order inversion:

```bash
./build/cororeplay_demo explore deadlock_lock_order \
  --seed 7 --runs 1 --output artifacts
```

This prints a `deadlock` result and writes:

```text
artifacts/deadlock_lock_order-run-0-seed-7191089600892374487/
├── artifact.json
└── trace.json
```

An exploration that finds a failure exits with status 1. Strictly replay it:

```bash
./build/cororeplay_demo replay deadlock_lock_order \
  --artifact artifacts/deadlock_lock_order-run-0-seed-7191089600892374487/artifact.json
```

Expected output:

```text
replay success decisions=4 events=19 terminal=deadlock
```

Replay exits 0 only when the metadata, every decision, every semantic event, terminal status, and final waiting snapshot match. It exits 1 for a structured divergence and 2 for command-line or I/O errors. Explore exits 0 when no failure is found, 1 when a failure is found, and 2 for usage/I/O errors. Replay never modifies the original artifact.

Import `trace.json` into Chrome Trace or [Perfetto UI](https://ui.perfetto.dev). Every semantic event is exported as a Chrome Trace instant, with stable Operation thread names and virtual-time timestamps. CTest also parses the generated file as JSON; visual inspection in Perfetto remains a manual release check.

### Built-in scenarios

| Case | Expected result with seed 7 | Purpose |
|---|---|---|
| `deadlock_lock_order` | `deadlock`, wait cycle | Two mutexes acquired in opposite orders |
| `invariant_lost_update` | `invariant_violation` | Read-yield-write loses an update |
| `invariant_lost_update_fixed` | `success` | Mutex-protected comparison control |
| `orphan_event_wait` | `orphan_wait` | Producer exits without setting the event |
| `no_progress` | `limit_exceeded/max_decisions` | Infinite yield loop stopped by an explicit limit |
| `exception_propagation` | `unhandled_exception` | Child throws after suspension and reaches root |
| `exception_propagation_caught` | `success` | Parent catches the child exception |

For example:

```bash
./build/cororeplay_demo explore invariant_lost_update --seed 7 --runs 1 --output artifacts
./build/cororeplay_demo explore orphan_event_wait --seed 7 --runs 1 --output artifacts
./build/cororeplay_demo explore no_progress --seed 7 --runs 1 --output artifacts --max-decisions 32
./build/cororeplay_demo explore exception_propagation --seed 7 --runs 1 --output artifacts
./build/cororeplay_demo explore invariant_lost_update_fixed --seed 7 --runs 1 --output artifacts
./build/cororeplay_demo explore exception_propagation_caught --seed 7 --runs 1 --output artifacts
```

Each failure has an independent directory and can be replayed by replacing the case and artifact path in the replay command. The CLI loads only these built-in scenarios; it does not load arbitrary external binaries.

## Public API overview

- `task<T>` / `task<void>`: lazy, move-only coroutine tasks; nested right-value `co_await` attaches children.
- `Runtime::spawn`, `make_mutex`, `make_manual_event`, `check_invariant`: scenario construction and checks.
- `yield()`, `sleep_for()`, `sleep_until()`: explicit suspension and virtual time.
- `explore(Scenario, ExploreOptions)`: seeded runs, structured per-run results, first-failure stop by default.
- `replay(Scenario, Artifact)`: structured success or `Divergence`, not just formatted text.
- `load_artifact`, `save_artifact`, `write_artifacts`, `export_chrome_trace`: versioned persistence and visualization.

A divergence reports its phase and index, expected/actual stable fields, current Operation and Resource state, and up to eight recent common events. Arbitrary business objects and platform-specific exception messages are not serialized. Invariants use explicit labels and stable integers.

`RuntimeConfig::trace_enabled` and `schedule_enabled` can disable recording for execution benchmarks. Both default to true and must remain enabled for failure artifacts and strict replay.

See [docs/design.md](docs/design.md) for the task state machine, ownership rules, deterministic algorithms, artifact schema, replay algorithm, and closed-world assumptions.

## Testing and regression artifacts

CTest includes unit tests, negative replay mutations, and all built-in explore/replay workflows. Coverage includes:

- Unstarted destruction, moved-from tasks, nested values, move-only results, exceptions, and blocked teardown.
- FIFO and seeded schedules, RNG golden vectors, virtual-time ordering, overflow, and all run limits.
- Mutex handoff/anti-barging, event generations, cross-Runtime misuse, deadlock cycles, and orphan waits.
- Selected/Ready changes, deleted events, changed mutex owners, truncated/extra records, terminal/version changes, and artifact byte immutability.
- One checked-in deadlock artifact at `tests/data/deadlock_artifact.json`, replayed 100 times per test run and by every CI platform.

## Benchmarks

Build benchmarks and run smoke workloads:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release \
  -DCOROREPLAY_BUILD_BENCHMARKS=ON
cmake --build build-release --parallel
./build-release/scheduler_bench 1000
./build-release/trace_replay_bench 128
```

The executables print compiler, build configuration, workload, trace/schedule switches, elapsed time, and throughput. CI runs only these smoke workloads and sets no timing threshold.

### Measured local results

Single-run measurements on 2026-11-10, Darwin 27.2.0 arm64, AppleClang 17.0.0, Release build:

`./build-release/scheduler_bench 20000`:

| Workload | Trace | Schedule | Elapsed | Reported throughput |
|---|---:|---:|---:|---:|
| 20,000 yields | off | off | 2.012 ms | 9,942,626 yields/s |
| 20,000 yields | on | off | 3.091 ms | 6,469,527 yields/s |
| 20,000 yields | on | on | 2.720 ms | 7,352,490 yields/s |
| 4 workers, 5,001 mutex iterations each | off | off | 2.576 ms | 7,766,408 acquisitions/s |

`./build-release/trace_replay_bench 2000`:

| Stage | Units | Elapsed |
|---|---:|---:|
| Explore with artifact export | 2,000 decisions | 40.231 ms |
| Artifact load | 6,003 events | 17.033 ms |
| Strict replay | 2,000 decisions | 0.337 ms |
| Artifact save | 6,003 events | 18.552 ms |
| Chrome Trace export | 6,003 events | 20.579 ms |
| Artifact and trace write | 6,003 events | 38.769 ms |

The trace was 2,305,131 bytes. These are observations from one local run, not guaranteed overhead ratios. In particular, the schedule-on sample above was faster than the schedule-off sample; no causal performance conclusion should be drawn without repeated measurements.

## CI

GitHub Actions defines Ubuntu GCC Debug/Release, Ubuntu Clang Debug, macOS AppleClang Debug/Release, and Linux ASan+UBSan jobs. Project warnings are errors. Every platform runs the same golden artifact replay, and failure logs/artifacts are uploaded.

## Limitations

- Closed-world integration only; arbitrary third-party coroutine types cannot be intercepted.
- Cooperative and non-preemptive: a loop without a suspension point cannot be stopped by scheduler limits.
- Seeded exploration is incomplete and is not a proof of correctness or liveness.
- No real I/O simulation, bounded channel, cancellation/select, worker pool, complete model checker, reverse process debugger, or custom GUI.
- No `task<T&>`, mutex RAII guard, cancellation, or timeout primitive in v0.1.
- Single-thread interleaving checks are not TSan and do not establish absence of real data races.
- Determinism requires scenario code to avoid external nondeterminism and to keep referenced state alive for the whole run.
