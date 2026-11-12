#include <cororeplay/runtime.hpp>

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

#include <cororeplay/artifact.hpp>
#include <cororeplay/async_mutex.hpp>
#include <cororeplay/manual_event.hpp>

#include <internal/scheduler.hpp>
#include <internal/trace.hpp>

namespace cororeplay {

namespace {

std::string number(std::uint64_t value) { return std::to_string(value); }

} // namespace

Runtime::Runtime(RuntimeConfig config) : impl_(std::make_unique<detail::RuntimeImpl>(config)) {
  impl_->owner = this;
}

Runtime::~Runtime() = default;

OperationId Runtime::spawn_handle(detail::TaskHandle root) {
  return impl_->attach(std::move(root), kNoOperation);
}

OperationId Runtime::attach_child(OperationId parent, detail::TaskHandle child) {
  return impl_->attach(std::move(child), parent);
}

async_mutex &Runtime::make_mutex() {
  if (impl_->ran) {
    throw UsageError("cannot create a resource after Runtime::run");
  }
  const auto id = static_cast<ResourceId>(impl_->resources.size() + 1);
  impl_->mutexes.push_back(std::unique_ptr<async_mutex>(new async_mutex(*this, id)));
  impl_->resources.push_back({detail::RuntimeImpl::ResourceKind::mutex, impl_->mutexes.size() - 1});
  return *impl_->mutexes.back();
}

manual_event &Runtime::make_manual_event() {
  if (impl_->ran) {
    throw UsageError("cannot create a resource after Runtime::run");
  }
  const auto id = static_cast<ResourceId>(impl_->resources.size() + 1);
  impl_->events.push_back(std::unique_ptr<manual_event>(new manual_event(*this, id)));
  impl_->resources.push_back({detail::RuntimeImpl::ResourceKind::event, impl_->events.size() - 1});
  return *impl_->events.back();
}

bool Runtime::check_invariant(std::string_view label, bool condition, std::int64_t observed,
                              std::int64_t expected) {
  if (impl_->ran && !impl_->running) {
    throw UsageError("cannot check an invariant after Runtime::run");
  }
  impl_->invariant(label, condition, observed, expected);
  return condition;
}

const RunRecord &Runtime::run() {
  if (impl_->ran) {
    throw UsageError("a Runtime can only be run once");
  }
  impl_->ran = true;
  detail::Scheduler scheduler(*impl_);
  scheduler.run();
  return impl_->record;
}

const RunRecord &Runtime::record() const noexcept { return impl_->record; }

VirtualTime Runtime::now() const noexcept { return impl_->clock; }

OperationState Runtime::state(OperationId operation) const {
  return impl_->operation(operation).state;
}

void *Runtime::take_result_address(OperationId operation) { return impl_->take_result(operation); }

void Runtime::rethrow_exception(OperationId operation) { impl_->rethrow_exception(operation); }

void Runtime::yield_current(OperationId operation) { impl_->yield(operation); }

void Runtime::sleep_current(OperationId operation, std::uint64_t value, bool until) {
  impl_->sleep(operation, value, until);
}

void Runtime::mutex_wait(OperationId operation, async_mutex &mutex) {
  impl_->mutex_wait(mutex, operation);
}

void Runtime::event_wait(OperationId operation, manual_event &event) {
  impl_->event_wait(event, operation);
}

bool Runtime::mutex_try_lock(async_mutex &mutex) { return impl_->try_lock(mutex, impl_->current); }

bool Runtime::event_wait_ready(manual_event &event) {
  return impl_->event_ready(event, impl_->current);
}

void Runtime::mutex_unlock(async_mutex &mutex) { impl_->mutex_unlock(mutex, impl_->current); }

void Runtime::event_set(manual_event &event) { impl_->event_set(event, impl_->current); }

void Runtime::event_reset(manual_event &event) { impl_->event_reset(event, impl_->current); }

void Runtime::configure_replay(const Artifact &artifact, Divergence &divergence) {
  impl_->replay_artifact = &artifact;
  impl_->divergence = &divergence;
}

namespace detail {

RuntimeImpl::RuntimeImpl(RuntimeConfig runtime_config)
    : config(runtime_config), random(runtime_config.seed) {}

thread_local RuntimeImpl *RuntimeImpl::active = nullptr;

RuntimeImpl::~RuntimeImpl() {
  for (auto &op : operations) {
    if (op.handle && op.destroy != nullptr) {
      op.destroy(op.handle);
      op.handle = {};
    }
  }
  operations.clear();
  mutexes.clear();
  events.clear();
}

Operation &RuntimeImpl::operation(OperationId id) {
  if (id == kNoOperation || id > operations.size()) {
    throw UsageError("invalid Operation ID " + number(id));
  }
  return operations[static_cast<std::size_t>(id - 1)];
}

const Operation &RuntimeImpl::operation(OperationId id) const {
  if (id == kNoOperation || id > operations.size()) {
    throw UsageError("invalid Operation ID " + number(id));
  }
  return operations[static_cast<std::size_t>(id - 1)];
}

OperationId RuntimeImpl::attach(TaskHandle task, OperationId parent) {
  if (ran && current == kNoOperation) {
    task.destroy(task.handle);
    throw UsageError("cannot spawn a root task after Runtime::run");
  }
  if (operations.size() >= config.limits.max_operations) {
    task.destroy(task.handle);
    set_terminal(FailureKind::limit_exceeded, "max_operations");
    return kNoOperation;
  }

  const auto id = static_cast<OperationId>(operations.size() + 1);
  Operation op;
  op.id = id;
  op.parent = parent;
  op.handle = task.handle;
  op.destroy = task.destroy;
  op.result_address = task.result_address;
  op.exception = task.exception;
  operations.push_back(std::move(op));
  try {
    task.bind(task.handle, *owner, id, parent);
  } catch (...) {
    operations.pop_back();
    task.destroy(task.handle);
    throw;
  }

  emit(EventKind::operation_create, id, parent);
  if (parent != kNoOperation) {
    auto &parent_op = operation(parent);
    if (parent_op.state != OperationState::running || parent_op.child != kNoOperation) {
      throw UsageError("invalid parent for child attach");
    }
    parent_op.child = id;
    emit(EventKind::child_attach, parent, id);
    make_waiting(parent, WaitKind::child, 0, 0, id);
  }
  make_ready(id, parent);
  return id;
}

void *RuntimeImpl::take_result(OperationId id) {
  auto &op = operation(id);
  if (op.state != OperationState::completed) {
    throw UsageError("operation result is not ready");
  }
  if (op.result_taken) {
    throw UsageError("operation result was already consumed");
  }
  op.result_taken = true;
  return op.result_address(op.handle);
}

void RuntimeImpl::rethrow_exception(OperationId id) {
  auto &op = operation(id);
  if (op.error) {
    std::rethrow_exception(op.error);
  }
}

void RuntimeImpl::require_current(OperationId id) const {
  if (id == kNoOperation || id != current) {
    throw UsageError("operation must be running in its owning Runtime");
  }
}

void RuntimeImpl::require_same_runtime(const Runtime *expected, OperationId id) const {
  if (expected != owner) {
    throw UsageError("resource used from a different Runtime");
  }
  require_current(id);
}

void RuntimeImpl::make_ready(OperationId id, OperationId related) {
  auto &op = operation(id);
  if (op.state == OperationState::completed) {
    throw UsageError("cannot ready a completed operation");
  }
  op.state = OperationState::ready;
  op.ready_order = ++next_ready_order;
  ready.insert(id);
  emit(EventKind::ready, id, related);
}

void RuntimeImpl::make_waiting(OperationId id, WaitKind kind, ResourceId resource, TimerId timer,
                               OperationId wait_for, std::uint64_t value) {
  auto &op = operation(id);
  op.state = OperationState::waiting;
  op.wait_kind = kind;
  op.resource = resource;
  op.timer = timer;
  op.wait_for = wait_for;
  op.wait_value = value;
  emit(EventKind::wait_start, id, wait_for, resource, timer, static_cast<std::int64_t>(value), 0,
       std::string(to_string(kind)));
}

void RuntimeImpl::yield(OperationId id) {
  require_current(id);
  make_ready(id);
}

void RuntimeImpl::sleep(OperationId id, std::uint64_t value, bool until) {
  require_current(id);
  auto deadline = clock.nanoseconds;
  if (until) {
    deadline = std::max(deadline, value);
  } else {
    if (value > std::numeric_limits<std::uint64_t>::max() - deadline) {
      make_waiting(id, WaitKind::timer);
      set_terminal(FailureKind::virtual_time_overflow, "virtual_time_overflow");
      return;
    }
    deadline += value;
  }
  const auto id_value = static_cast<TimerId>(timers.size() + 1);
  timers.push_back({id_value, id, VirtualTime{deadline}, true});
  timer_queue.insert({deadline, id_value});
  emit(EventKind::timer_arm, id, kNoOperation, 0, id_value);
  make_waiting(id, WaitKind::timer, 0, id_value);
}

bool RuntimeImpl::try_lock(async_mutex &mutex, OperationId id) {
  require_same_runtime(mutex.runtime_, id);
  if (mutex.owner_ == id) {
    throw UsageError("non-recursive async_mutex self-lock");
  }
  if (mutex.owner_ != kNoOperation) {
    return false;
  }
  if (!mutex.waiters_.empty()) {
    throw UsageError("async_mutex waiter queue has no logical owner");
  }
  mutex.owner_ = id;
  emit(EventKind::mutex_acquire, id, kNoOperation, mutex.id_, 0, static_cast<std::int64_t>(id));
  return true;
}

void RuntimeImpl::mutex_wait(async_mutex &mutex, OperationId id) {
  require_same_runtime(mutex.runtime_, id);
  if (mutex.owner_ == id) {
    throw UsageError("non-recursive async_mutex self-lock");
  }
  if (mutex.owner_ == kNoOperation) {
    throw UsageError("async_mutex fast path was not attempted");
  }
  mutex.waiters_.push_back(id);
  make_waiting(id, WaitKind::mutex, mutex.id_, 0, mutex.owner_);
}

void RuntimeImpl::mutex_unlock(async_mutex &mutex, OperationId id) {
  require_same_runtime(mutex.runtime_, id);
  if (mutex.owner_ != id) {
    throw UsageError("async_mutex unlocked by a non-owner");
  }
  if (mutex.waiters_.empty()) {
    mutex.owner_ = kNoOperation;
    emit(EventKind::mutex_release, id, kNoOperation, mutex.id_);
    return;
  }

  const auto next = mutex.waiters_.front();
  mutex.waiters_.pop_front();
  mutex.owner_ = next;
  for (const auto waiter : mutex.waiters_) {
    operation(waiter).wait_for = next;
  }
  emit(EventKind::mutex_release, id, next, mutex.id_);
  emit(EventKind::mutex_handoff, id, next, mutex.id_, 0, static_cast<std::int64_t>(next));
  make_ready(next, id);
}

bool RuntimeImpl::event_ready(manual_event &event, OperationId id) const {
  require_same_runtime(event.runtime_, id);
  return event.set_;
}

void RuntimeImpl::event_wait(manual_event &event, OperationId id) {
  require_same_runtime(event.runtime_, id);
  if (event.set_) {
    throw UsageError("manual_event fast path was not attempted");
  }
  event.waiters_.push_back(id);
  make_waiting(id, WaitKind::event, event.id_, 0, kNoOperation, event.generation_);
}

void RuntimeImpl::event_set(manual_event &event, OperationId id) {
  if (ran && !running) {
    throw UsageError("cannot mutate a manual_event after Runtime::run");
  }
  if (active != nullptr && active != this) {
    throw UsageError("manual_event set from a different Runtime");
  }
  if (event.runtime_ != owner) {
    throw UsageError("resource used from a different Runtime");
  }
  if (event.generation_ == std::numeric_limits<std::uint64_t>::max()) {
    set_terminal(FailureKind::virtual_time_overflow, "event_generation_overflow");
    return;
  }
  ++event.generation_;
  event.set_ = true;
  emit(EventKind::event_set, id, kNoOperation, event.id_, 0,
       static_cast<std::int64_t>(event.generation_));
  auto waiters = std::exchange(event.waiters_, {});
  for (const auto waiter : waiters) {
    make_ready(waiter, id);
  }
}

void RuntimeImpl::event_reset(manual_event &event, OperationId id) {
  if (ran && !running) {
    throw UsageError("cannot mutate a manual_event after Runtime::run");
  }
  if (active != nullptr && active != this) {
    throw UsageError("manual_event reset from a different Runtime");
  }
  if (event.runtime_ != owner) {
    throw UsageError("resource used from a different Runtime");
  }
  if (event.generation_ == std::numeric_limits<std::uint64_t>::max()) {
    set_terminal(FailureKind::virtual_time_overflow, "event_generation_overflow");
    return;
  }
  ++event.generation_;
  event.set_ = false;
  emit(EventKind::event_reset, id, kNoOperation, event.id_, 0,
       static_cast<std::int64_t>(event.generation_));
}

void RuntimeImpl::invariant(std::string_view label, bool condition, std::int64_t observed,
                            std::int64_t expected) {
  emit(EventKind::invariant, current, kNoOperation, 0, 0, observed, expected, std::string(label));
  if (!condition) {
    set_terminal(FailureKind::invariant_violation, "invariant:" + std::string(label));
  }
}

void RuntimeImpl::set_terminal(FailureKind kind, std::string code) {
  if (record.terminal.kind == FailureKind::none) {
    record.terminal = {kind, std::move(code)};
  }
}

void RuntimeImpl::snapshot_waits() {
  record.waiting.clear();
  for (const auto &op : operations) {
    if (op.state == OperationState::waiting) {
      record.waiting.push_back({op.id, op.wait_kind, op.wait_for, op.resource, op.timer});
    }
  }
}

bool RuntimeImpl::has_wait_cycle() {
  std::vector<unsigned char> color(operations.size() + 1, 0);
  for (const auto &start : operations) {
    if (start.state != OperationState::waiting) {
      continue;
    }
    auto id = start.id;
    while (id != kNoOperation && id <= operations.size()) {
      if (color[static_cast<std::size_t>(id)] == 1) {
        return true;
      }
      if (color[static_cast<std::size_t>(id)] == 2) {
        break;
      }
      color[static_cast<std::size_t>(id)] = 1;
      const auto &op = operation(id);
      if (op.state != OperationState::waiting ||
          (op.wait_kind != WaitKind::child && op.wait_kind != WaitKind::mutex)) {
        break;
      }
      id = op.wait_for;
    }
    for (auto &op : operations) {
      if (color[static_cast<std::size_t>(op.id)] == 1) {
        color[static_cast<std::size_t>(op.id)] = 2;
      }
    }
  }
  return false;
}

std::string RuntimeImpl::operation_description(OperationId id) const {
  if (id == kNoOperation) {
    return "none";
  }
  if (id > operations.size()) {
    return "invalid:" + number(id);
  }
  const auto &op = operation(id);
  std::ostringstream out;
  out << "op=" << id << " state=" << to_string(op.state);
  if (op.state == OperationState::waiting || op.state == OperationState::ready) {
    out << " wait=" << to_string(op.wait_kind) << " wait_for=" << op.wait_for
        << " resource=" << op.resource << " timer=" << op.timer;
  }
  return out.str();
}

std::string RuntimeImpl::resource_description(ResourceId id) const {
  if (id == 0) {
    return "none";
  }
  if (id > resources.size()) {
    return "invalid:" + number(id);
  }
  const auto &info = resources[static_cast<std::size_t>(id - 1)];
  std::ostringstream out;
  if (info.kind == ResourceKind::mutex) {
    const auto &mutex = *mutexes[info.index];
    out << "mutex=" << id << " owner=" << mutex.owner_ << " waiters=[";
    for (const auto waiter : mutex.waiters_) {
      out << waiter << ',';
    }
    out << ']';
  } else {
    const auto &event = *events[info.index];
    out << "event=" << id << " set=" << event.set_ << " generation=" << event.generation_
        << " waiters=[";
    for (const auto waiter : event.waiters_) {
      out << waiter << ',';
    }
    out << ']';
  }
  return out.str();
}

} // namespace detail
} // namespace cororeplay
