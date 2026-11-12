#pragma once

#include <coroutine>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace cororeplay {

class Runtime;
class async_mutex;
class manual_event;

using OperationId = std::uint64_t;
using ResourceId = std::uint64_t;
using TimerId = std::uint64_t;
inline constexpr OperationId kNoOperation = 0;
inline constexpr std::uint64_t kNoDecision = UINT64_MAX;

class UsageError : public std::logic_error {
public:
  using std::logic_error::logic_error;
};

namespace detail {

struct TaskHandle {
  std::coroutine_handle<> handle;
  void (*bind)(std::coroutine_handle<>, Runtime &, OperationId, OperationId);
  void (*destroy)(std::coroutine_handle<>);
  void *(*result_address)(std::coroutine_handle<>);
  std::exception_ptr (*exception)(std::coroutine_handle<>);
};

class promise_base {
public:
  Runtime &runtime() const;
  OperationId operation_id() const noexcept { return operation_id_; }
  OperationId parent_id() const noexcept { return parent_id_; }
  bool attached() const noexcept { return attached_; }

  void yield_await();
  void sleep_await(std::uint64_t value, bool until);
  void mutex_await(async_mutex &mutex);
  void event_await(manual_event &event);
  OperationId attach_child(TaskHandle child);
  void *take_child_result(OperationId child);
  void rethrow_child_exception(OperationId child);

  Runtime *runtime_ = nullptr;
  OperationId operation_id_ = kNoOperation;
  OperationId parent_id_ = kNoOperation;
  bool attached_ = false;
  std::exception_ptr exception_;

  void bind(Runtime &runtime, OperationId id, OperationId parent);

  template <typename T> friend class task;
  friend class Runtime;
};

struct YieldAwaitable {
  bool await_ready() const noexcept { return false; }

  template <typename Promise> void await_suspend(std::coroutine_handle<Promise> handle) const {
    handle.promise().yield_await();
  }

  void await_resume() const noexcept {}
};

} // namespace detail

inline detail::YieldAwaitable yield() noexcept { return {}; }

template <typename T = void> class task;

template <typename T> class task {
public:
  static_assert(!std::is_reference_v<T>, "task<T&> is not supported");

  struct promise_type;
  using handle_type = std::coroutine_handle<promise_type>;

  task() noexcept = default;
  task(const task &) = delete;
  task &operator=(const task &) = delete;
  task(task &&other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  task &operator=(task &&other) noexcept;
  ~task();

  bool valid() const noexcept { return static_cast<bool>(handle_); }

  class Awaiter;
  Awaiter operator co_await() &&;
  Awaiter operator co_await() & = delete;

private:
  explicit task(handle_type handle) : handle_(handle) {}
  void reset();
  detail::TaskHandle release();

  handle_type handle_;
  friend class Runtime;
};

template <typename T> struct task<T>::promise_type : detail::promise_base {
  task get_return_object() { return task(handle_type::from_promise(*this)); }
  std::suspend_always initial_suspend() const noexcept { return {}; }
  std::suspend_always final_suspend() const noexcept { return {}; }
  void return_value(T value) { result_.emplace(std::move(value)); }
  void unhandled_exception() noexcept { exception_ = std::current_exception(); }

  std::optional<T> result_;
};

template <typename T> task<T> &task<T>::operator=(task &&other) noexcept {
  if (this != &other) {
    reset();
    handle_ = std::exchange(other.handle_, {});
  }
  return *this;
}

template <typename T> task<T>::~task() { reset(); }

template <typename T> void task<T>::reset() {
  if (handle_ && !handle_.promise().attached()) {
    handle_.destroy();
  }
  handle_ = {};
}

template <typename T> detail::TaskHandle task<T>::release() {
  if (!handle_) {
    throw UsageError("cannot attach an empty task");
  }
  auto handle = std::exchange(handle_, {});
  return {
      handle,
      [](std::coroutine_handle<> erased, Runtime &runtime, OperationId id, OperationId parent) {
        auto typed = handle_type::from_address(erased.address());
        typed.promise().bind(runtime, id, parent);
      },
      [](std::coroutine_handle<> erased) { handle_type::from_address(erased.address()).destroy(); },
      [](std::coroutine_handle<> erased) -> void * {
        auto typed = handle_type::from_address(erased.address());
        return typed.promise().result_.has_value() ? std::addressof(*typed.promise().result_)
                                                   : nullptr;
      },
      [](std::coroutine_handle<> erased) -> std::exception_ptr {
        return handle_type::from_address(erased.address()).promise().exception_;
      },
  };
}

template <typename T> class task<T>::Awaiter {
public:
  explicit Awaiter(task &&child) : child_(std::move(child)) {}

  bool await_ready() const noexcept { return false; }

  template <typename Promise> void await_suspend(std::coroutine_handle<Promise> parent) {
    parent_ = &parent.promise();
    child_id_ = parent_->attach_child(child_.release());
  }

  T await_resume() {
    parent_->rethrow_child_exception(child_id_);
    return std::move(*static_cast<T *>(parent_->take_child_result(child_id_)));
  }

private:
  task child_;
  detail::promise_base *parent_ = nullptr;
  OperationId child_id_ = kNoOperation;
};

template <typename T> typename task<T>::Awaiter task<T>::operator co_await() && {
  return Awaiter(std::move(*this));
}

template <> class task<void> {
public:
  struct promise_type;
  using handle_type = std::coroutine_handle<promise_type>;

  task() noexcept = default;
  task(const task &) = delete;
  task &operator=(const task &) = delete;
  task(task &&other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  task &operator=(task &&other) noexcept;
  ~task();

  bool valid() const noexcept { return static_cast<bool>(handle_); }

  class Awaiter;
  Awaiter operator co_await() &&;
  Awaiter operator co_await() & = delete;

private:
  explicit task(handle_type handle) : handle_(handle) {}
  void reset();
  detail::TaskHandle release();

  handle_type handle_;
  friend class Runtime;
};

struct task<void>::promise_type : detail::promise_base {
  task get_return_object() { return task(handle_type::from_promise(*this)); }
  std::suspend_always initial_suspend() const noexcept { return {}; }
  std::suspend_always final_suspend() const noexcept { return {}; }
  void return_void() const noexcept {}
  void unhandled_exception() noexcept { exception_ = std::current_exception(); }
};

inline task<void> &task<void>::operator=(task &&other) noexcept {
  if (this != &other) {
    reset();
    handle_ = std::exchange(other.handle_, {});
  }
  return *this;
}

inline task<void>::~task() { reset(); }

inline void task<void>::reset() {
  if (handle_ && !handle_.promise().attached()) {
    handle_.destroy();
  }
  handle_ = {};
}

inline detail::TaskHandle task<void>::release() {
  if (!handle_) {
    throw UsageError("cannot attach an empty task");
  }
  auto handle = std::exchange(handle_, {});
  return {
      handle,
      [](std::coroutine_handle<> erased, Runtime &runtime, OperationId id, OperationId parent) {
        auto typed = handle_type::from_address(erased.address());
        typed.promise().bind(runtime, id, parent);
      },
      [](std::coroutine_handle<> erased) { handle_type::from_address(erased.address()).destroy(); },
      [](std::coroutine_handle<>) -> void * { return nullptr; },
      [](std::coroutine_handle<> erased) -> std::exception_ptr {
        return handle_type::from_address(erased.address()).promise().exception_;
      },
  };
}

class task<void>::Awaiter {
public:
  explicit Awaiter(task &&child) : child_(std::move(child)) {}

  bool await_ready() const noexcept { return false; }

  template <typename Promise> void await_suspend(std::coroutine_handle<Promise> parent) {
    parent_ = &parent.promise();
    child_id_ = parent_->attach_child(child_.release());
  }

  void await_resume() {
    parent_->rethrow_child_exception(child_id_);
    parent_->take_child_result(child_id_);
  }

private:
  task child_;
  detail::promise_base *parent_ = nullptr;
  OperationId child_id_ = kNoOperation;
};

inline task<void>::Awaiter task<void>::operator co_await() && { return Awaiter(std::move(*this)); }

} // namespace cororeplay
