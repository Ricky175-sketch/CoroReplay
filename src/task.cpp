#include <cororeplay/task.hpp>

#include <cororeplay/async_mutex.hpp>
#include <cororeplay/manual_event.hpp>
#include <cororeplay/runtime.hpp>

namespace cororeplay::detail {

Runtime &promise_base::runtime() const {
  if (runtime_ == nullptr) {
    throw UsageError("task is not attached to a Runtime");
  }
  return *runtime_;
}

void promise_base::bind(Runtime &runtime, OperationId id, OperationId parent) {
  if (attached_ || runtime_ != nullptr) {
    throw UsageError("task is already attached");
  }
  runtime_ = &runtime;
  operation_id_ = id;
  parent_id_ = parent;
  attached_ = true;
}

void promise_base::yield_await() { runtime().yield_current(operation_id_); }

void promise_base::sleep_await(std::uint64_t value, bool until) {
  runtime().sleep_current(operation_id_, value, until);
}

void promise_base::mutex_await(async_mutex &mutex) { runtime().mutex_wait(operation_id_, mutex); }

void promise_base::event_await(manual_event &event) { runtime().event_wait(operation_id_, event); }

OperationId promise_base::attach_child(TaskHandle child) {
  return runtime().attach_child(operation_id_, std::move(child));
}

void *promise_base::take_child_result(OperationId child) {
  return runtime().take_result_address(child);
}

void promise_base::rethrow_child_exception(OperationId child) {
  runtime().rethrow_exception(child);
}

} // namespace cororeplay::detail
