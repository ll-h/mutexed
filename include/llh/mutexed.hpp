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


struct has_cv {};
struct no_cv {};

template<typename M, typename H = no_cv>
struct mutexed_base{};

template<typename M>
struct mutexed_base<M, has_cv> {
    std::condition_variable_any mutable cv_;
};

template<>
struct mutexed_base<std::mutex, has_cv> {
    std::condition_variable mutable cv_;
};

/** tag used to provide arguments for the in-place construction of the inner mutex */
struct mutex_args_t{};
/** tag used to provide arguments for the in-place construction of the mutexed value */
struct value_args_t{};

template<typename Tag, typename... MutexArgs>
constexpr bool contains_tag() {
    return ((std::is_same_v<std::decay_t<MutexArgs>, Tag>) || ...);
}

template<typename Tag, typename... T>
concept does_not_contain_tag = !contains_tag<Tag, T...>();

template<typename T, typename M = std::shared_mutex, typename H = no_cv>
class Mutexed : public mutexed_tag, private mutexed_base<M, H> {
private:
    M mutable mtx_;
    T val_;

    friend all_locker;

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

    using wait_lock = std::conditional_t<
        std::is_same_v<M, std::mutex> || !shared_lockable<M>,
        std::unique_lock<M>,
        std::shared_lock<M>
    >;

public:
    using value_type = T;
    using mutex_type = M;

    /** A shared_lock<M> if M is shared_lockable, a unique_lock<M> otherwise */
    using possibly_shared_lock = std::conditional_t<
        shared_lockable<M>,
        std::shared_lock<M>,
        std::unique_lock<M>
    >;

    Mutexed(Mutexed&&) = delete;
    Mutexed(Mutexed const&) = delete;

    template<typename... ValueArgs>
    requires does_not_contain_tag<mutex_args_t, ValueArgs...> &&
        std::is_constructible_v<T, ValueArgs&&...>
    explicit Mutexed(ValueArgs&&... args) : mtx_(), val_(std::forward<ValueArgs>(args)...) {}

    template<typename ValArg, typename MutexArg>
    explicit Mutexed(ValArg&& v_arg, MutexArg&& m_arg) :
        mtx_(std::forward<MutexArg>(m_arg)),
        val_(std::forward<ValArg>(v_arg))
    {}

    template<typename... MutexArgs>
    requires does_not_contain_tag<value_args_t, MutexArgs...>
    explicit Mutexed(mutex_args_t, MutexArgs&&... m_args) :
        mtx_(std::forward<MutexArgs>(m_args)...),
        val_()
    {}

    template<typename F>
    requires
        invokable_with<F, T const&> ||
        invokable_with<F, T> && std::is_copy_constructible_v<T>
    decltype(auto) with_locked(F&& f) const {
        possibly_shared_lock lock(mtx_);
        return std::invoke(std::forward<F>(f), val_);
    }

    template<typename F>
    requires invokable_with<F, T&>
    decltype(auto) with_locked(F&& f) {
        notifier dn(*this);
        std::lock_guard lock(mtx_);
        return std::invoke(f, val_);
    }

    template<typename = void>
    requires std::is_copy_constructible_v<T>
    T get_copy() const {
        possibly_shared_lock lock(mtx_);
        return val_;
    }

    template<typename Predicate>
    requires std::is_same_v<H, has_cv> && invokable_with<Predicate, T const&>
    void wait(Predicate&& p) const {
        wait_lock lock(mtx_);
        this->cv_.wait(lock, [p = std::forward<Predicate>(p), this](){ return std::invoke(p, val_); });
    }

    template<class Rep, class Period, typename Predicate>
    requires std::is_same_v<H, has_cv> && invokable_with<Predicate, T const&>
    bool wait_for(std::chrono::duration<Rep, Period> const& rel_time, Predicate&& p) const {
        wait_lock lock(mtx_);
        return this->cv_.wait_for(lock, rel_time, [p = std::forward<Predicate>(p), this](){ return std::invoke(p, val_); });
    }

    template<class Clock, class Duration, typename Predicate>
    requires std::is_same_v<H, has_cv> && invokable_with<Predicate, T const&>
    bool wait_until(std::chrono::time_point<Clock, Duration> const& timeout_time, Predicate&& p) const {
        wait_lock lock(mtx_);
        return this->cv_.wait_until(lock, timeout_time, [p = std::forward<Predicate>(p), this](){ return std::invoke(p, val_); });
    }


    decltype(auto) locked() {
        struct Lock {
            Mutexed& m;

            void lock()   { m.mtx_.lock(); }
            void unlock() { m.mtx_.unlock(); }
            bool try_lock() { return m.mtx_.try_lock(); }

            explicit Lock(Mutexed& mtx) : m(mtx) { lock(); }

            ~Lock() {
                unlock();
                if constexpr (std::is_same_v<H, has_cv>) {
                    m.cv_.notify_all();
                }
            }
        };
        return std::tuple<Lock, T&>(*this, val_);
    }
    std::tuple<possibly_shared_lock, T const&> locked() const {
        return locked_const();
    }
    std::tuple<possibly_shared_lock, T const&> locked_const() const {
        return std::tuple<possibly_shared_lock, T const&>{mtx_, val_};
    }
};

} // end namespace details

using details::Mutexed;
using details::has_cv;
using details::no_cv;

inline details::mutex_args_t mutex_args{};
inline details::value_args_t value_args{};

inline details::all_locker with_all_locked{};

using details::shared_lockable;

} // end namespace llh::mutexed
