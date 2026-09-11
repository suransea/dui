#pragma once

#include <coroutine>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace dui {

namespace detail {

struct CoroutineControl {
    std::coroutine_handle<> coroutine;
    bool started{};

    ~CoroutineControl() {
        if (coroutine != nullptr) {
            coroutine.destroy();
        }
    }
};

struct ExecutorState {
    std::deque<std::weak_ptr<CoroutineControl>> pending;
};

} // namespace detail

class ManualExecutor {
public:
    class ScheduleAwaitable {
    public:
        explicit ScheduleAwaitable(std::weak_ptr<detail::ExecutorState> state) :
            state_(std::move(state)) {}

        [[nodiscard]] bool await_ready() const noexcept { return false; }

        template<class Promise>
        void await_suspend(std::coroutine_handle<Promise> coroutine) const {
            const auto state = state_.lock();
            const auto control = coroutine.promise().control_token().lock();
            if (state == nullptr || control == nullptr) {
                throw std::logic_error("Cannot schedule on a destroyed ManualExecutor");
            }
            state->pending.push_back(control);
        }

        void await_resume() const noexcept {}

    private:
        std::weak_ptr<detail::ExecutorState> state_;
    };

    ManualExecutor() : state_(std::make_shared<detail::ExecutorState>()) {}

    [[nodiscard]] ScheduleAwaitable schedule() { return ScheduleAwaitable{state_}; }
    [[nodiscard]] bool run_one();
    void run_all();
    [[nodiscard]] std::size_t pending_count() const { return state_->pending.size(); }

private:
    static bool run_one(const std::shared_ptr<detail::ExecutorState>& state);

    std::shared_ptr<detail::ExecutorState> state_;
};

template<class T>
class Task {
public:
    struct promise_type {
        [[nodiscard]] Task get_return_object() {
            auto coroutine = std::coroutine_handle<promise_type>::from_promise(*this);
            auto owner = std::make_shared<detail::CoroutineControl>();
            owner->coroutine = coroutine;
            control = owner;
            return Task{std::move(owner)};
        }

        [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }
        [[nodiscard]] std::suspend_always final_suspend() const noexcept { return {}; }

        template<class U>
        void return_value(U&& value) {
            result.emplace(std::forward<U>(value));
        }

        void unhandled_exception() noexcept { exception = std::current_exception(); }
        [[nodiscard]] std::weak_ptr<detail::CoroutineControl> control_token() const {
            return control;
        }

        std::optional<T> result;
        std::exception_ptr exception;
        std::weak_ptr<detail::CoroutineControl> control;
    };

    Task() = default;
    ~Task() = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;

    void start() {
        const auto control = require_control();
        if (control->started) {
            return;
        }
        control->started = true;
        control->coroutine.resume();
    }

    [[nodiscard]] bool started() const {
        return control_ != nullptr && control_->started;
    }

    [[nodiscard]] bool done() const {
        return control_ != nullptr && control_->coroutine.done();
    }

    [[nodiscard]] const T& result() const {
        const auto control = require_control();
        if (!control->coroutine.done()) {
            throw std::logic_error("Task result is not ready");
        }
        const promise_type& promise = typed_handle(*control).promise();
        if (promise.exception != nullptr) {
            std::rethrow_exception(promise.exception);
        }
        return *promise.result;
    }

private:
    explicit Task(std::shared_ptr<detail::CoroutineControl> control) :
        control_(std::move(control)) {}

    [[nodiscard]] std::shared_ptr<detail::CoroutineControl> require_control() const {
        if (control_ == nullptr) {
            throw std::logic_error("Task is empty");
        }
        return control_;
    }

    [[nodiscard]] static std::coroutine_handle<promise_type> typed_handle(
        const detail::CoroutineControl& control
    ) {
        return std::coroutine_handle<promise_type>::from_address(control.coroutine.address());
    }

    std::shared_ptr<detail::CoroutineControl> control_;
};

template<>
class Task<void> {
public:
    struct promise_type {
        [[nodiscard]] Task get_return_object() {
            auto coroutine = std::coroutine_handle<promise_type>::from_promise(*this);
            auto owner = std::make_shared<detail::CoroutineControl>();
            owner->coroutine = coroutine;
            control = owner;
            return Task{std::move(owner)};
        }

        [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }
        [[nodiscard]] std::suspend_always final_suspend() const noexcept { return {}; }
        void return_void() const noexcept {}
        void unhandled_exception() noexcept { exception = std::current_exception(); }
        [[nodiscard]] std::weak_ptr<detail::CoroutineControl> control_token() const {
            return control;
        }

        std::exception_ptr exception;
        std::weak_ptr<detail::CoroutineControl> control;
    };

    Task() = default;
    ~Task() = default;
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;

    void start() {
        const auto control = require_control();
        if (control->started) {
            return;
        }
        control->started = true;
        control->coroutine.resume();
    }

    [[nodiscard]] bool started() const {
        return control_ != nullptr && control_->started;
    }

    [[nodiscard]] bool done() const {
        return control_ != nullptr && control_->coroutine.done();
    }

    void result() const {
        const auto control = require_control();
        if (!control->coroutine.done()) {
            throw std::logic_error("Task result is not ready");
        }
        const promise_type& promise = typed_handle(*control).promise();
        if (promise.exception != nullptr) {
            std::rethrow_exception(promise.exception);
        }
    }

private:
    explicit Task(std::shared_ptr<detail::CoroutineControl> control) :
        control_(std::move(control)) {}

    [[nodiscard]] std::shared_ptr<detail::CoroutineControl> require_control() const {
        if (control_ == nullptr) {
            throw std::logic_error("Task is empty");
        }
        return control_;
    }

    [[nodiscard]] static std::coroutine_handle<promise_type> typed_handle(
        const detail::CoroutineControl& control
    ) {
        return std::coroutine_handle<promise_type>::from_address(control.coroutine.address());
    }

    std::shared_ptr<detail::CoroutineControl> control_;
};

} // namespace dui
