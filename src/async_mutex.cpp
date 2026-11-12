#include <cororeplay/async_mutex.hpp>

#include <cororeplay/runtime.hpp>

namespace cororeplay {

bool async_mutex::LockAwaiter::await_ready() const { return mutex_->lock_ready(); }

bool async_mutex::lock_ready() { return runtime_->mutex_try_lock(*this); }

void async_mutex::unlock() { runtime_->mutex_unlock(*this); }

} // namespace cororeplay
