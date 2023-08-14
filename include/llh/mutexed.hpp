#pragma once

#include <condition_variable>
#include <shared_mutex>
#include <mutex>
#include <type_traits>
#include <utility>
#include <functional>

namespace llh::mutexed {
namespace details {

template<typename F, typename A>
concept invokable_with = requires (F&& f, A a) {
    std::invoke(std::forward<F>(f), a);
};

template<typename M>
concept shared_lockable = requires(M& m) {
    m.lock_shared();
    m.unlock_shared();
    { m.try_lock_shared() } -> std::same_as<bool>;
};

struct mutexed_tag {};

template<typename T>
struct decay_through_ref_wrap {
    using type = std::decay_t<T>;
};

template<typename T>
struct decay_through_ref_wrap<std::reference_wrapper<T>> {
    using type = std::decay_t<T>;
};

template<typename T>
using decay_through_ref_wrap_t = typename decay_through_ref_wrap<T>::type;


inline struct all_locker {
    template<typename M>
    struct lockable_proxy {
        M& m;

        void lock() { m.mtx_.lock(); }
        void unlock() { m.mtx_.unlock(); }
        bool try_lock() { return m.mtx_.try_lock(); }

        auto& inner_val_ref() { return m.val_; }
    };

    template<typename M>
    requires shared_lockable<typename M::mutex_type>
    struct lockable_proxy<M const> {
        M& m;

        explicit lockable_proxy(M const& c_m) : m(const_cast<M&>(c_m)) {}

        void lock() { m.mtx_.lock_shared(); }
        void unlock() { m.mtx_.unlock_shared(); }
        bool try_lock() { return m.mtx_.try_lock_shared(); }

        auto const& inner_val_ref() { return m.val_; }
    };

    template<typename M> lockable_proxy(std::reference_wrapper<M>) -> lockable_proxy<M>;
    template<typename M> lockable_proxy(M&) -> lockable_proxy<M>;

    template<typename F, typename... M>
    requires std::conjunction_v<std::is_base_of<mutexed_tag, decay_through_ref_wrap_t<M>>...>
    decltype(auto) operator()(F&& f, M&&... mtxs) const {
        return [](auto&& f, auto&&... mp) {
            std::scoped_lock<std::decay_t<decltype(mp)>...> lock(mp...);
            return std::invoke(std::forward<F>(f), mp.inner_val_ref()...);
        }(std::forward<F>(f), lockable_proxy{std::forward<M>(mtxs)}...);
    }
} with_all_locked{};


template<typename M>
struct mutex_wrapper {
    M mutable mtx_;

    using lock_type = std::unique_lock<M>;

    void lock_shared()     { mtx_.lock(); }
    void lock()            { mtx_.lock(); }
    void unlock()          { mtx_.unlock(); }
    void unlock_shared()   { mtx_.unlock(); }
    bool try_lock_shared() { return mtx_.try_lock(); }
    bool try_lock()        { return mtx_.try_lock(); }
};

template<typename M>
requires shared_lockable<M>
struct mutex_wrapper<M> {
    M mutable mtx_;

    using lock_type = std::shared_lock<M>;

    void lock_shared()     { mtx_.lock_shared(); }
    void lock()            { mtx_.lock_shared(); }
    void unlock()          { mtx_.unlock_shared(); }
    void unlock_shared()   { mtx_.unlock_shared(); }
    bool try_lock_shared() { return mtx_.try_lock_shared(); }
    bool try_lock()        { return mtx_.try_lock_shared(); }
};

struct has_cv {};
struct no_cv {};

template<typename M, typename H = no_cv>
struct mutexed_base : mutex_wrapper<M> {};

template<typename M>
struct mutexed_base<M, has_cv> : mutex_wrapper<M> {
    std::condition_variable_any mutable cv_;
};

template<>
struct mutexed_base<std::mutex, has_cv> : mutex_wrapper<std::mutex> {
    std::condition_variable mutable cv_;
};

template<typename T, typename M = std::shared_mutex, typename H = no_cv>
class Mutexed : public mutexed_tag, private mutexed_base<M, H> {
private:
    T val_;

    friend all_locker;

    mutex_wrapper<M>& as_wrapper() const {
        // mutex_wrapper just wraps a mutable mutex so its methods are callable in const contexts
        return *const_cast<Mutexed*>(this);
    }

    template<typename DoesNotHaveCV>
    struct defer_notify{
        template<typename Ignored>
        explicit defer_notify(Ignored&&) {}
    };

    template<typename HasCV>
    requires requires(HasCV& m) { m.cv_; }
    struct defer_notify<HasCV> {
        decltype(std::declval<HasCV>().cv_)& cv_;

        explicit defer_notify(HasCV const& m) : cv_(m.cv_) {}

        ~defer_notify() {
            cv_.notify_all();
        }
    };

    using notifier = defer_notify<Mutexed>;
    using wait_lock_type = std::conditional_t<
        std::is_same_v<M, std::mutex>,
        std::unique_lock<mutex_wrapper<M>>,
        std::shared_lock<mutex_wrapper<M>>
    >;

    // Creates a lock guard that uses lock_shared() except if M==std::mutex.
    // Mendatory copy elision makes `auto lock = wait_lock();` only lock the mutex once
    auto possibly_shared_lock() const {
        if constexpr (std::is_same_v<M, std::mutex>)
            return std::unique_lock(this->mtx_);
        else
            return std::shared_lock(as_wrapper());
    }

public:
    using value_type = T;
    using mutex_type = M;

    Mutexed(Mutexed&&) = delete;
    Mutexed(Mutexed const&) = delete;

    template<typename... Args>
    explicit Mutexed(Args... args) : val_(std::forward<Args>(args)...) {}

    template<typename F>
    requires
        invokable_with<F, T const&> ||
        invokable_with<F, T> && std::is_copy_constructible_v<T>
    decltype(auto) with_locked(F&& f) const {
        std::shared_lock lock(as_wrapper());
        return std::invoke(std::forward<F>(f), val_);
    }

    template<typename F>
    requires invokable_with<F, T&>
    decltype(auto) with_locked(F&& f) {
        notifier dn(*this);
        std::lock_guard lock(this->mtx_);
        return std::invoke(f, val_);
    }

    template<typename = void>
    requires std::is_copy_constructible_v<T>
    T get_copy() const {
        std::shared_lock lock(as_wrapper());
        return val_;
    }

    template<typename Predicate>
    requires std::is_same_v<H, has_cv> && invokable_with<Predicate, T const&>
    void wait(Predicate&& p) const {
        auto lock = possibly_shared_lock();
        this->cv_.wait(lock, [p = std::forward<Predicate>(p), this](){ return std::invoke(p, val_); });
    }

    template<class Rep, class Period, typename Predicate>
    requires std::is_same_v<H, has_cv> && invokable_with<Predicate, T const&>
    bool wait_for(std::chrono::duration<Rep, Period> const& rel_time, Predicate&& p) const {
        auto lock = possibly_shared_lock();
        return this->cv_.wait_for(lock, rel_time, [p = std::forward<Predicate>(p), this](){ return std::invoke(p, val_); });
    }

    template<class Clock, class Duration, typename Predicate>
    requires std::is_same_v<H, has_cv> && invokable_with<Predicate, T const&>
    bool wait_until(std::chrono::time_point<Clock, Duration> const& timeout_time, Predicate&& p) const {
        auto lock = possibly_shared_lock();
        return this->cv_.wait_until(lock, timeout_time, [p = std::forward<Predicate>(p), this](){ return std::invoke(p, val_); });
    }


    decltype(auto) locked() {
        struct Lock {
            Mutexed& m;

            explicit Lock(Mutexed& mtx) : m(mtx) {
                m.mtx_.lock();
            }

            ~Lock() {
                m.mtx_.unlock();
                if constexpr (std::is_same_v<H, has_cv>) {
                    m.cv_.notify_all();
                }
            }
        };
        return std::tuple<Lock, T&>(*this, val_);
    }
    decltype(auto) locked() const {
        return std::tuple<decltype(possibly_shared_lock()), T const&>(possibly_shared_lock(), val_);
    }
    decltype(auto) locked_const() const {
        return locked();
    }
};

} // end namespace details

using details::Mutexed;
using details::has_cv;
using details::no_cv;

inline details::all_locker with_all_locked{};

} // end namespace llh::mutexed
