// SPDX-FileCopyrightText: 2021 Daniel Vrátil <dvratil@kde.org>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "coroutine.h"

#include <variant>
#include <atomic>

#include <QtGlobal>

namespace QCoro
{

template<typename T = void>
class Task;

namespace detail {

template<typename T, typename = void>
struct awaiter_type;

template<typename T>
using awaiter_type_t = typename awaiter_type<T>::type;

//! An awaiter that resumes a co_awaiting coroutine when the co_awaited coroutine finishes.
class TaskFinalSuspend {
public:
    //! Constructs the awaitable, passing it a handle to the co_awaiting coroutine
    /*!
     * \param[in] awaitingCoroutine handle of the coroutine that is co_awaiting the current
     * coroutine.
     */
    explicit TaskFinalSuspend(QCORO_STD::coroutine_handle<> awaitingCoroutine)
        : mAwaitingCoroutine(awaitingCoroutine) {}

    //! Forces the compiler to suspend the just-finished coroutine.
    constexpr bool await_ready() const noexcept {
        return false;
    }

    //! Called by the compiler when the just-finished coroutine is suspended.
    /*!
     * It is given handle of the coroutine that is being co_awaited (that is the current
     * coroutine). If there is a co_awaiting coroutine and it has not been resumed yet,
     * it resumes it.
     *
     * \param[in] awaitedCoroutine handle of the coroutine being co_awaited by this Awaitable.
     */
    template<typename _Promise>
    void await_suspend(QCORO_STD::coroutine_handle<_Promise> awaitedCoroutine) noexcept {
        auto &promise = awaitedCoroutine.promise();

        if (promise.mResumeAwaiter.exchange(true, std::memory_order_acq_rel)) {
            promise.mAwaitingCoroutine.resume();
        }
    }

    //! Called by the compiler when the just-finished coroutine should be resumed.
    /*!
     * In reality this should never be called, as our coroutine is finished so it won't be resumed.
     * In any case, this method does nothing.
     * */
    void await_resume() const noexcept {}

private:
    //! Handle of the coroutine co_awaiting the current coroutine
    QCORO_STD::coroutine_handle<> mAwaitingCoroutine;
};

//! Base class for the \c Task<T> promise_type.
/*!
 * This is a promise_type for a Task<T> returned from a coroutine. When a coroutine
 * is constructed, it looks at its return type, which will be \c Task<T>, and constructs
 * new object of type \c Task<T>::promise_type (which will be \c TaskPromise<T>). Using
 * \c TaskPromise<T>::get_return_object() it obtains a new object of Task<T>. Then the
 * coroutine user code is executed and runs until the it reaches a suspend point - either
 * a co_await keyword, co_return or until it reaches the end of user code.
 *
 * You can think about promise as an interface that is callee-facing, while Task<T> is
 * an caller-facing interface (in respect to the current coroutine).
 *
 * Promise interface must provide several methods:
 *  * get_return_object() - it is called by the compiler at the very beginning of a coroutine
 *    and is used to obtain the object that will be returned from the coroutine whenever it is
 *    suspended.
 *  * initial_suspend() - it is co_awaited by the code generated immediately before user
 *    code. Depending on the Awaitable that it returns, the coroutine will either suspend
 *    and the user code will only be executed once it is co_awaited by some other coroutine,
 *    or it will begin executing the user code immediately. In case of QCoro, the promise always
 *    returns std::suspend_never, which is a standard library awaitable, which prevents the
 *    coroutine from being suspended at the beginning.
 *  * final_suspend() - it is co_awaited when the coroutine co_returns, or when it reaches
 *    the end of user code. Same as with initial_suspend(), depending on the type of Awaitable
 *    it returns, it either suspends the coroutine (and then it must be destroyed explicitly
 *    by the Awaiter), or resumes and takes care of destroying the frame pointer. In case of
 *    QCoro, the promise returns a custom Awaitable called TaskFinalSuspend, which, when
 *    co_awaited by the compiler-generated code will make sure that if there is a coroutine
 *    co_awaiting on the current corutine, that the co_awaiting coroutine is resumed.
 *  * unhandled_exception() - called by the compiler if the coroutine throws an unhandled
 *    exception. The promise will store the exception and it will be rethrown when the
 *    co_awaiting coroutine tries to retrieve a result of the coroutine that has thrown.
 *  * return_value() - called by the compiler to store co_returned result of the function.
 *    It must only be present if the coroutine is not void.
 *  * return_void() - called by the compiler when the coroutine co_returns or flows of the
 *    end of user code. It must only be present if the coroutine return type is void.
 *  * await_transform() - this one is optional and is used by co_awaits inside the coroutine.
 *    It allows the promise to transform the co_awaited type to an Awaitable.
 */
class TaskPromiseBase {
public:
    //! Called when the coroutine is started to decide whether it should be suspended or not.
    /*!
     * We want coroutines that return QCoro::Task<T> to start automatically, because it will
     * likely be executed from Qt's event loop, which will not co_await it, but rather call
     * it as a regular function.
     * */
    constexpr QCORO_STD::suspend_never initial_suspend() const noexcept { return {}; }
    //! Called when the coroutine co_returns or reaches the end of user code.
    /*!
     * This decides what should happen when the coroutine is finished.
     */
    auto final_suspend() const noexcept { return TaskFinalSuspend{mAwaitingCoroutine}; }

    //! Called by co_await to obtain an Awaitable for type \c T.
    /*!
     * When co_awaiting on a value of type \c T, the type \c T must an Awaitable. To allow
     * to co_await even on types that are not Awaitable (e.g. 3rd party types like QNetworkReply),
     * C++ allows promise_type to provide \c await_transform() function that can transform
     * the type \c T into an Awaitable. This is a very powerful mechanism in C++ coroutines.
     *
     * For types \c T for which there is no valid await_transform() overload, the C++ attempts
     * to use those types directly as Awaitables. This is a perfectly valid scenario in cases
     * when co_awaiting a type that implements the neccessary Awaitable interface.
     *
     * In our implementation, the await_transform() is overloaded only for Qt types for which
     * a specialiation of the \c QCoro::detail::awaiter_type template class exists. The
     * specialization returns type of the Awaiter for the given type \c T.
     */
    template<typename T,
             typename Awaiter = QCoro::detail::awaiter_type_t<std::remove_cvref_t<T>>>
    auto await_transform(T &&value) {
        return Awaiter{value};
    }

    //! Specialized overload of await_transform() for Task<T>.
    /*!
     *
     * When co_awaiting on a value of type \c Task<T>, the co_await will try to obtain
     * an Awaitable object for the \c Task<T>. Since \c Task<T> already implements the
     * Awaitable interface it can be directly used as an Awaitable, thus this specialization
     * only acts as an identity operation.
     *
     * The reason we need to specialize it is that co_await will always call promise's await_transform()
     * overload if it exists. Since our generic await_transform() overload exists only for Qt types
     * that specialize the \c QCoro::detail::awaiter_type template, the compilation would fail
     * due to no suitable overload for \c QCoro::Task<T> being found.
     *
     * The reason the compiler doesn't check for an existence of await_transform() overload for type T
     * and falling back to using the T as an Awaitable, but instead only checks for existence of
     * any overload of await_transform() and failing compilation if non of the overloads is suitable
     * for type T is, that it allows us to delete await_transform() overload for a particular type,
     * and thus disallow that type to be co_awaited, even if the type itself provides the Awaitable
     * interface. This would not be possible if the compiler would always fall back to using the T
     * as an Awaitable type.
     */
    template<typename T>
    auto await_transform(Task<T> &&task) {
        return std::forward<Task<T>>(task);
    }

    //! Called by \c TaskAwaiter when co_awaited.
    /*!
     * This function is called by a TaskAwaiter, e.g. an object obtain by co_await
     * when a value of Task<T> is co_awaited (in other words, when a coroutine co_awaits on
     * another coroutine returning Task<T>).
     *
     * \param awaitingCoroutine handle for the coroutine that is co_awaiting on a coroutine that
     *                          represented by this promise. When our coroutine finishes, it's
     *                          our job to resume the awaiting coroutine.
     */
    bool setAwaitingCoroutine(QCORO_STD::coroutine_handle<> awaitingCoroutine) {
        mAwaitingCoroutine = awaitingCoroutine;
        return !mResumeAwaiter.exchange(true, std::memory_order_acq_rel);
    }

private:
    friend class TaskFinalSuspend;
    //! Handle of the coroutine that is currently co_awaiting this Awaitable
    QCORO_STD::coroutine_handle<> mAwaitingCoroutine;
    //! Indicates whether the awaiter should be resumed when it tries to co_await on us.
    std::atomic<bool> mResumeAwaiter{false};
};

//! The promise_type for Task<T>
/*!
 * See \ref TaskPromiseBase documentation for explanation about promise_type.
 */
template<typename T>
class TaskPromise final: public TaskPromiseBase {
public:
    //! Constructs a Task<T> for this promise.
    Task<T> get_return_object() noexcept;

    void unhandled_exception() {
        mValue = std::current_exception();
    }

    void return_value(T &&value) noexcept {
        mValue = std::forward<T>(value);
    }

    T &result() & {
        if (std::holds_alternative<std::exception_ptr>(mValue)) {
            std::rethrow_exception(std::get<std::exception_ptr>(mValue));
        }

        return std::get<T>(mValue);
    }

    T &&result() && {
        if (std::holds_alternative<std::exception_ptr>(mValue)) {
            std::rethrow_exception(std::get<std::exception_ptr>(mValue));
        }

        return std::move(std::get<T>(mValue));
    }

private:
    //! Holds either the return value of the coroutine or exception thrown by the coroutine.
    std::variant<std::monostate, T, std::exception_ptr> mValue;
};

//! Specialization of TaskPromise for coroutines returning \c void.
template<>
class TaskPromise<void> final: public TaskPromiseBase {
public:
    //! \copydoc TaskPromise<T>::get_return_object()
    Task<void> get_return_object() noexcept;

    //! \copydoc TaskPromise<T>::unhandled_exception()
    void unhandled_exception() {
        mException = std::current_exception();
    }

    //! Promise type must have this function when the coroutine return type is void.
    void return_void() noexcept {};

    //! Provides access to the result of the coroutine.
    /*!
     * Since this is a promise type for a void coroutine, the only result that
     * this can return is re-throwing an exception thrown by the coroutine, if
     * there's any.
     */
    void result() {
        if (mException) {
            std::rethrow_exception(mException);
        }
    }

private:
    //! Exception thrown by the coroutine.
    std::exception_ptr mException;
};


//! Base-class for Awaiter objects returned by the \c Task<T> operator co_await().
template<typename _Promise>
class TaskAwaiterBase {
public:
    //! Returns whether to co_await
    bool await_ready() const noexcept {
        return !mAwaitedCoroutine || mAwaitedCoroutine.done();
    }

    //! Called by co_await in a coroutine that co_awaits our awaited coroutine managed by the current task.
    /*!
     * In other words, let's have a case like this:
     * \code{.cpp}
     * Task<> doSomething() {
     *    ...
     *    co_return result;
     * };
     *
     * Task<> getSomething() {
     *    ...
     *    const auto something = co_await doSomething();
     *    ...
     * }
     * \endcode
     *
     * If this Awaiter object is an awaiter of the doSomething() coroutine (e.g. has been constructed
     * by the co_await), then \c mAwaitedCoroutine is the handle of the doSomething() coroutine,
     * and \c awaitingCoroutine is a handle of the getSomething() coroutine which is awaiting the
     * completion of the doSomething() coroutine.
     *
     * This is implemented by passing the awaiting coroutine handle to the promise of the
     * awaited coroutine. When the awaited coroutine finishes, the promise will take care of
     * resuming the awaiting coroutine. At the same time this function resumes the awaited
     * coroutine.
     *
     * \param[in] awaitingCoroutine handle of the coroutine that is currently co_awaiting the
     * coroutine represented by this Tak.
     * \return returns whether the awaiting coroutine should be suspended, or whether the
     * co_awaited coroutine has finished synchronously and the co_awaiting coroutine doesn't
     * have to suspend.
     */
     bool await_suspend(QCORO_STD::coroutine_handle<> awaitingCoroutine) noexcept {
        mAwaitedCoroutine.resume();
        return mAwaitedCoroutine.promise().setAwaitingCoroutine(awaitingCoroutine);
    }

protected:
    //! Constucts a new Awaiter object.
    /*!
     * \param[in] coroutine hande for the coroutine that is being co_awaited.
     */
    TaskAwaiterBase(QCORO_STD::coroutine_handle<_Promise> awaitedCoroutine)
        : mAwaitedCoroutine(awaitedCoroutine) {}

    //! Handle of the coroutine that is being co_awaited by this awaiter
    QCORO_STD::coroutine_handle<_Promise> mAwaitedCoroutine = {};
};

} // namespace detail

//! An asynchronously executed task.
/*!
 * When a coroutine is called which has  return type Task<T>, the coroutine will
 * construct a new instance of Task<T> which will be returned to the caller when
 * the coroutine is suspended - that is either when it co_awaits another coroutine
 * of finishes executing user code.
 *
 * In the sense of the interface that the task implements, it is an Awaitable.
 *
 * Task<T> is constructed by a code generated at the beginning of the coroutine by
 * the compiler (i.e. before the user code). The code first creates a new frame
 * pointer, which internally holds the promise. The promise is of type \c R::promise_type,
 * where \c R is the return type of the function (so \c Task<T>, in our case.
 *
 * One can think about it as Task being the caller-facing interface and promise being
 * the callee-facing interface.
 */
template<typename T>
class Task {
public:
    using promise_type = detail::TaskPromise<T>;
    using value_type = T;

    //! Constructs a new empty task.
    explicit Task() noexcept = default;

    //! Constructs a task bound to a coroutine.
    /*!
     * \param[in] coroutine handle of the coroutine that has constructed the task.
     */
    explicit Task(QCORO_STD::coroutine_handle<promise_type> coroutine)
        : mCoroutine(coroutine)
    {}

    //! Task cannot be copy-constructed.
    Task(const Task &) = delete;
    //! Task cannot be copy-assigned.
    Task &operator=(const Task &) = delete;

    //! The task can be move-constructed.
    Task(Task &&other) noexcept
        : mCoroutine(other.mCoroutine) {
        other.mCoroutine = nullptr;
    }

    //! The task can be move-assigned.
    Task &operator=(Task &&other) noexcept {
        if (mCoroutine) {
            mCoroutine.destroy();
        }

        mCoroutine = other.mCoroutine;
        other.mCoroutine = nullptr;
        return *this;
    }

    //! Destructor.
    /*!
     * If the task is bound to a coroutine, it ensures that the coroutine
     * frame pointer is destroyed as well. We can do this, because if the
     * task is being destroyed it implies that the coroutine has finished
     * as well.
     */
    ~Task() {
        if (mCoroutine) {
            mCoroutine.destroy();
        }
    }

    //! Returns whether the task has finished.
    /*!
     * A task that is ready (represents a finished coroutine) must not attempt
     * to suspend the coroutine again.
     */
    const bool isReady() const {
        return !mCoroutine || mCoroutine.done();
    }

    //! Provides an Awaiter for the coroutine machinery.
    /*!
     * The coroutine machinery looks for a suitable operator co_await overload
     * for the current Awaitable (this Task). It calls it to obtain an Awaiter
     * object, that is an object that the co_await keyword uses to suspend and
     * resume the coroutine.
     */
    auto operator co_await() const & noexcept {
        //! Specialization of the TaskAwaiterBase that returns the promise result by value
        class TaskAwaiter:  public detail::TaskAwaiterBase<promise_type> {
        public:
            TaskAwaiter(QCORO_STD::coroutine_handle<promise_type> awaitedCoroutine)
                : detail::TaskAwaiterBase<promise_type>{awaitedCoroutine} {}

            //! Called when the co_awaited coroutine is resumed.
            /*
             * \return the result from the coroutine's promise, factically the
             * value co_returned by the coroutine. */
            auto await_resume() {
                Q_ASSERT(this->mAwaitedCoroutine != nullptr);
                return this->mAwaitedCoroutine.promise().result();
            }
        };
        return TaskAwaiter{mCoroutine};
    }

    //! \copydoc operator co_await() const & noexcept
    auto operator co_await() const && noexcept {
        //! Specialization of the TaskAwaiterBase that returns the promise result as an r-value reference.
        class TaskAwaiter : public detail::TaskAwaiterBase<promise_type> {
        public:
            TaskAwaiter(QCORO_STD::coroutine_handle<promise_type> awaitedCoroutine)
                : detail::TaskAwaiterBase<promise_type>{awaitedCoroutine} {}

            //! Called when the co_awaited coroutine is resumed.
            /*
             * \return an r-value reference to the coroutine's promise result, factically
             *  a value co_returned by the coroutine. */
            auto await_resume() {
                Q_ASSERT(this->mAwaitedCoroutine != nullptr);
                return std::move(this->mAwaitedCoroutine.promise().result());
            }
        };
        return TaskAwaiter{mCoroutine};
    }

private:
    //! The coroutine represented by this task
    /*!
     * In other words, this is a handle to the coroutine that has constructed and
     * returned this Task<T>.
     * */
    QCORO_STD::coroutine_handle<promise_type> mCoroutine = {};
};


namespace detail {

template<typename T>
Task<T> TaskPromise<T>::get_return_object() noexcept{
    return Task<T>{QCORO_STD::coroutine_handle<TaskPromise>::from_promise(*this)};
}

Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>{QCORO_STD::coroutine_handle<TaskPromise>::from_promise(*this)};
}

} // namespace detail

} // namespace QCoro

