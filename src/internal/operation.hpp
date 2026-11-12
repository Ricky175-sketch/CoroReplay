#pragma once

#include <coroutine>
#include <exception>

#include <cororeplay/runtime.hpp>

namespace cororeplay::detail {

struct Operation {
  OperationId id = kNoOperation;
  OperationId parent = kNoOperation;
  OperationId child = kNoOperation;
  std::coroutine_handle<> handle;
  void (*destroy)(std::coroutine_handle<>) = nullptr;
  void *(*result_address)(std::coroutine_handle<>) = nullptr;
  std::exception_ptr (*exception)(std::coroutine_handle<>) = nullptr;
  OperationState state = OperationState::created;
  WaitKind wait_kind = WaitKind::none;
  OperationId wait_for = kNoOperation;
  ResourceId resource = 0;
  TimerId timer = 0;
  std::uint64_t wait_value = 0;
  std::uint64_t ready_order = 0;
  std::exception_ptr error;
  bool result_taken = false;
};

struct Timer {
  TimerId id = 0;
  OperationId operation = kNoOperation;
  VirtualTime deadline;
  bool armed = true;
};

} // namespace cororeplay::detail
