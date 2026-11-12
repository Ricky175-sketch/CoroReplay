cmake_minimum_required(VERSION 3.24)
project(CoroReplay VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)
include(CTest)

option(COROREPLAY_BUILD_TESTING "Build CoroReplay tests" OFF)
option(COROREPLAY_BUILD_EXAMPLES "Build CoroReplay demos" ON)
option(COROREPLAY_BUILD_BENCHMARKS "Build CoroReplay benchmarks" OFF)
option(COROREPLAY_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(COROREPLAY_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

function(cororeplay_project_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if(COROREPLAY_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(COROREPLAY_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(cororeplay
  src/task.cpp
  src/runtime.cpp
  src/scheduler.cpp
  src/time.cpp
  src/async_mutex.cpp
  src/manual_event.cpp
  src/schedule.cpp
  src/trace.cpp
  src/explore.cpp
  src/artifact.cpp
  src/chrome_trace.cpp
)
add_library(cororeplay::cororeplay ALIAS cororeplay)
target_compile_features(cororeplay PUBLIC cxx_std_20)
target_include_directories(cororeplay
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(cororeplay PRIVATE nlohmann_json::nlohmann_json)
cororeplay_project_warnings(cororeplay)

if(COROREPLAY_ENABLE_SANITIZERS)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(cororeplay PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(cororeplay PUBLIC -fsanitize=address,undefined)
  else()
    message(FATAL_ERROR "COROREPLAY_ENABLE_SANITIZERS requires GCC or Clang")
  endif()
endif()

if(COROREPLAY_BUILD_EXAMPLES OR COROREPLAY_BUILD_TESTING OR COROREPLAY_BUILD_BENCHMARKS)
  add_library(cororeplay_scenarios
    examples/deadlock_lock_order.cpp
    examples/invariant_lost_update.cpp
    examples/orphan_event_wait.cpp
    examples/no_progress.cpp
    examples/exception_propagation.cpp
  )
  target_include_directories(cororeplay_scenarios PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/examples)
  target_link_libraries(cororeplay_scenarios PUBLIC cororeplay::cororeplay)
  cororeplay_project_warnings(cororeplay_scenarios)
endif()

if(COROREPLAY_BUILD_EXAMPLES)
  add_executable(cororeplay_demo tools/cororeplay_demo/main.cpp)
  target_link_libraries(cororeplay_demo PRIVATE cororeplay_scenarios)
  cororeplay_project_warnings(cororeplay_demo)
endif()

if(COROREPLAY_BUILD_TESTING)
  FetchContent_Declare(
    Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz
    URL_HASH SHA256=c991b247a1a0d7bb9c39aa35faf0fe9e19764213f28ffba3109388e62ee0269c
  )
  FetchContent_MakeAvailable(Catch2)

  add_executable(cororeplay_tests
    tests/task_tests.cpp
    tests/scheduler_tests.cpp
    tests/virtual_time_tests.cpp
    tests/primitives_tests.cpp
    tests/replay_tests.cpp
    tests/artifact_tests.cpp
    tests/trace_export_tests.cpp
  )
  target_include_directories(cororeplay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/examples)
  target_link_libraries(cororeplay_tests PRIVATE cororeplay_scenarios Catch2::Catch2WithMain)
  target_compile_definitions(cororeplay_tests PRIVATE COROREPLAY_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  cororeplay_project_warnings(cororeplay_tests)
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  include(Catch)
  catch_discover_tests(cororeplay_tests)

  if(COROREPLAY_BUILD_EXAMPLES)
    set(COROREPLAY_DEMO_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/demo-artifacts)
    set(COROREPLAY_DEMO_EFFECTIVE_SEED 7191089600892374487)

    function(cororeplay_add_failure_demo scenario terminal)
      set(directory ${COROREPLAY_DEMO_OUTPUT}/${scenario}-run-0-seed-${COROREPLAY_DEMO_EFFECTIVE_SEED})
      set(explore_test demo_${scenario}_explore)
      set(replay_test demo_${scenario}_replay)
      add_test(NAME ${explore_test}
        COMMAND $<TARGET_FILE:cororeplay_demo> explore ${scenario}
          --seed 7 --runs 1 --output ${COROREPLAY_DEMO_OUTPUT} ${ARGN})
      set_tests_properties(${explore_test} PROPERTIES WILL_FAIL TRUE)
      add_test(NAME ${replay_test}
        COMMAND $<TARGET_FILE:cororeplay_demo> replay ${scenario} --artifact ${directory}/artifact.json)
      set_tests_properties(${replay_test} PROPERTIES
        DEPENDS ${explore_test}
        PASS_REGULAR_EXPRESSION "terminal=${terminal}")
    endfunction()

    cororeplay_add_failure_demo(deadlock_lock_order deadlock)
    cororeplay_add_failure_demo(invariant_lost_update invariant_violation)
    cororeplay_add_failure_demo(orphan_event_wait orphan_wait)
    cororeplay_add_failure_demo(no_progress limit_exceeded --max-decisions 32)
    cororeplay_add_failure_demo(exception_propagation unhandled_exception)

    add_test(NAME demo_invariant_lost_update_fixed
      COMMAND $<TARGET_FILE:cororeplay_demo> explore invariant_lost_update_fixed
        --seed 7 --runs 1 --output ${COROREPLAY_DEMO_OUTPUT})
    set_tests_properties(demo_invariant_lost_update_fixed PROPERTIES
      PASS_REGULAR_EXPRESSION "terminal=success")
    add_test(NAME demo_exception_propagation_caught
      COMMAND $<TARGET_FILE:cororeplay_demo> explore exception_propagation_caught
        --seed 7 --runs 1 --output ${COROREPLAY_DEMO_OUTPUT})
    set_tests_properties(demo_exception_propagation_caught PROPERTIES
      PASS_REGULAR_EXPRESSION "terminal=success")
    add_test(NAME demo_golden_cross_platform_replay
      COMMAND $<TARGET_FILE:cororeplay_demo> replay deadlock_lock_order
        --artifact ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/deadlock_artifact.json)

    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
      add_test(NAME demo_chrome_trace_json
        COMMAND ${Python3_EXECUTABLE} -c
          "import json,sys; d=json.load(open(sys.argv[1])); assert isinstance(d.get('traceEvents'),list); assert any(e.get('ph')=='i' for e in d['traceEvents']); assert all('name' in e and 'ph' in e for e in d['traceEvents'])"
          ${COROREPLAY_DEMO_OUTPUT}/deadlock_lock_order-run-0-seed-${COROREPLAY_DEMO_EFFECTIVE_SEED}/trace.json)
      set_tests_properties(demo_chrome_trace_json PROPERTIES DEPENDS demo_deadlock_lock_order_explore)
    endif()
  endif()
endif()

if(COROREPLAY_BUILD_BENCHMARKS)
  add_executable(scheduler_bench benchmarks/scheduler_bench.cpp)
  target_link_libraries(scheduler_bench PRIVATE cororeplay::cororeplay)
  cororeplay_project_warnings(scheduler_bench)
  add_executable(trace_replay_bench benchmarks/trace_replay_bench.cpp)
  target_include_directories(trace_replay_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/examples)
  target_link_libraries(trace_replay_bench PRIVATE cororeplay_scenarios)
  cororeplay_project_warnings(trace_replay_bench)
endif()
