#pragma once

#include <condition_variable>
#include <shared_mutex>
#include <mutex>
#include <type_traits>
#include <utility>
#include <functional>

namespace llh::mutexed {

//! Checks the invokability of F with a value of type A
template<typename F, typename A>
concept invokable_with = requires (F&& f, A a) {
    std::invoke(std::forward<F>(f), a);
};

//! Checks if M has the member functions of a shared mutex.
template<typename M>
concept shared_lockable = requires(M& m) {
    m.lock_shared();
    m.unlock_shared();
    { m.try_lock_shared() } -> std::same_as<bool>;
};


//! A tag type to use as last template argument of Mutexed to enable the *waiting API* but making it handle a **condition-variable**.
struct has_cv {};

//! The default last template argument of Mutexed, disabling the *waiting API* but not pay its costs.
struct no_cv {};

//! Checks if @a Tag is in @a Ts
template<typename Tag, typename... Ts>
constexpr bool contains_tag() {
    return ((std::is_same_v<std::decay_t<Ts>, Tag>) || ...);
}

//! Checks if @a Tag is not in @a Ts
template<typename Tag, typename... Ts>
concept does_not_contain_tag = !contains_tag<Tag, Ts...>();


namespace details {

//! A tag for identifying Mutexed classes.
struct mutexed_tag {};

template<typename T>
struct decay_through_ref_wrap {
    using type = std::decay_t<T>;
};

template<typename T>
struct decay_through_ref_wrap<std::reference_wrapper<T>> {
    using type = std::decay_t<T>;
};

//! Strips a type of its **cv-qualifiers** and if it is a reference or `std::reference_wrapper`, returns the underlying type.
template<typename T>
using decay_through_ref_wrap_t = typename decay_through_ref_wrap<T>::type;


/* Functor that locks all provided Mutexed for the duration of a provided function.
   It was implemented this way instead of a being directly a free function because it needs
   access to the private members of Mutexed, and writing `friend details::all_locker`
   is easier than copy-pasting the whole signature of the function.
 */
struct all_locker {
    // This specialization simply forwards the calls to the `lock()` functions
    // to the inner mutex of the held `Mutexed&`.
    template<typename M>
    struct lockable_proxy {
        M& m;

        void lock() { m.mtx_.lock(); }
        void unlock() { m.mtx_.unlock(); }
        bool try_lock() { return m.mtx_.try_lock(); }

        auto& inner_val_ref() { return m.val_; }
    };

    /* This specialization calls the `lock_shared()` functions on the inner mutex
       of the held `Mutexed&` whenever their not-shared counterpart is called.

       It applies when Mutexed::mutex_type is shared_lockable and when the template
       parameter M is const.
     */
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
        /* If we just invoke f, only lock() or try_lock() will be called on the mutexes.
           Instead, we create a lockable_proxy of the Mutexed s that will dispatch the
           calls made by std::lock() to their shared counterparts when it is suitable.

           Because std::lock() takes references, we need lockable_proxy variables
           somewhere. This implementation puts them as arguments of a lambda that is
           instantly called.
         */
        return [](auto&& f, auto&&... mp) {
            std::scoped_lock<std::decay_t<decltype(mp)>...> lock(mp...);
            return std::invoke(std::forward<F>(f), mp.inner_val_ref()...);
        }(std::forward<F>(f), lockable_proxy{std::forward<M>(mtxs)}...);
    }
};


/** The base class of Mutexed that handles the possession and type of a condition-variable member. */
template<typename M, typename H = no_cv>
struct mutexed_base{};

template<typename M>
struct mutexed_base<M, has_cv> {
    std::condition_variable_any mutable cv_;
};

//! `std::condition_variable` is faster but only works for `std::mutex`,
//! so we make a specialization for it.
template<>
struct mutexed_base<std::mutex, has_cv> {
    std::condition_variable mutable cv_;
};

} // end namespace details

//! Disambiguation tag type used to provide arguments for the in-place construction of the inner mutex.
struct mutex_args_t{};
//! Disambiguation tag type used to provide arguments for the in-place construction of the mutexed value.
struct value_args_t{};


/** The Mutexed class is a value-wrapper that protects its value with a mutex
 * that will be referred to in this documentation as the <em>inner mutex</em>.
 *
 * @tparam T the type of the <em>wrapped value</em>.
 * @tparam M the type of the <em>inner mutex</em>.
 *         If it is @link llh::mutexed::shared_lockable shared_lockable @endlink
 *         , @a read-access to the <em>wrapped value</em> is done by using the
 *         `lock_shared()` function of the <em>inner mutex</em>.
 * @tparam H option to activate @ref Waiting if it is has_cv.
 *         The default value is no_cv, in which case no @a condition-variable is
 *         held and waiting functions are not available.
 */
template<typename T, typename M = std::shared_mutex, typename H = no_cv>
class Mutexed : private details::mutexed_tag, private details::mutexed_base<M, H> {
private:
    M mutable mtx_;
    T val_;

    friend details::all_locker;

    //! A struct that notifies the **condition-variable** of a Mutexed if it has one.
    //! The default case for the template parameter gives a struct that does nothing.
    template<typename DoesNotHaveCV>
    struct defer_notify {
        //! This constructor does nothing.
        template<typename Ignored>
        explicit defer_notify(Ignored&&) {}
    };

    //! This specialization's destructor calls `notify_all()` on a **condition-variable**.
    template<typename HasCV>
    requires requires(HasCV& m) { m.cv_.notify_all(); }
    struct defer_notify<HasCV> {
        decltype(std::declval<HasCV>().cv_)& cv_;

        explicit defer_notify(HasCV const& m) : cv_(m.cv_) {}

        ~defer_notify() {
            cv_.notify_all();
        }
    };

    using notifier = defer_notify<Mutexed>;

public:
    //! The type of the wrapped value
    using value_type = T;
    //! The type of the <em>inner mutex</em>
    using mutex_type = M;

    //! A `std::shared_lock<M>` if Mutexed::mutex_type is @link
    //! llh::mutexed::shared_lockable shared_lockable @endlink, a
    //! `std::unique_lock<M>` otherwise.
    using possibly_shared_lock = std::conditional_t<
        shared_lockable<M>,
        std::shared_lock<M>,
        std::unique_lock<M>
    >;

    Mutexed(Mutexed&&) = delete;
    Mutexed(Mutexed const&) = delete;

    //! In-place-constructs the wrapped value with the provided arguments
    //! and default-initializes the mutex.
    template<typename... ValueArgs>
    requires does_not_contain_tag<mutex_args_t, ValueArgs...> &&
        std::is_constructible_v<T, ValueArgs&&...>
    explicit Mutexed(ValueArgs&&... args) : mtx_(), val_(std::forward<ValueArgs>(args)...) {}

    //! Forwards the first argument to the constructor of the value and
    //! the second argument to the constructor of the mutex.
    template<typename ValArg, typename MutexArg>
    explicit Mutexed(ValArg&& v_arg, MutexArg&& m_arg) :
        mtx_(std::forward<MutexArg>(m_arg)),
        val_(std::forward<ValArg>(v_arg))
    {}

    //! In-place-constructs the mutex with the provided arguments
    //! and default-initializes the wrapped value.
    template<typename... MutexArgs>
    requires does_not_contain_tag<value_args_t, MutexArgs...>
    explicit Mutexed(mutex_args_t, MutexArgs&&... m_args) :
        mtx_(std::forward<MutexArgs>(m_args)...),
        val_()
    {}

    /** Calls @a f with a `const&` or a copy of the wrapped value while locking
     *  the <em>inner mutex</em>.
     *
     * If the <em>inner mutex</em> is @link llh::mutexed::shared_lockable
     * shared_lockable @endlink, `lock_shared()` will be used.
     *
     * This overload is chosen if @c this is @c const or if @c f is @link
     * llh::mutexed::invokable_with invokable_with @endlink either @ref
     * value_type or a @c const& to it.
     *
     * Example usage :
     * ```cpp
     * struct read_me {
     *     int val = 42;
     *     int value() const { return val; }
     * };
     * llh::mutexed::Mutexed<read_me> protected_int;
     *
     * // with a const member function :
     * std::cout << protected_int.with_locked(&read_me::value) << std::endl;
     *
     * // with a lambda taking a const ref :
     * std::cout << protected_int.with_locked([](auto const& rm){ return rm.val; }) << std::endl;
     *
     * // with a lambda taking an auto but from a const Mutexed :
     * std::cout << std::cref(protected_int).get().with_locked([](auto rm){ return rm.val; }) << std::endl;
     * ```
     *
     * @param f The functor that will be called with the wrapped value while
     *          the <em>inner mutex</em> will be locked.
     */
    template<typename F>
    requires
        invokable_with<F, T const&> ||
        invokable_with<F, T> && std::is_copy_constructible_v<T>
    decltype(auto) with_locked(F&& f) const {
        possibly_shared_lock lock(mtx_);
        return std::invoke(std::forward<F>(f), val_);
    }


    /** Calls @a f with a reference on the wrapped value while locking the
     *  <em>inner mutex</em>.
     * 
     * If @ref Waiting is enabled, the @a inner condition-variable is notified
     * with `notify_all()` after the <em>inner mutex</em> is unlocked.
     *
     * This overload is chosen if @c this is not @c const and if @c f is @link
     * llh::mutexed::invokable_with invokable_with @endlink a non-<c>const</c>
     * reference to @ref value_type.
     *
     * Example usage :
     * ```cpp
     * llh::mutexed::Mutexed<int> protected_int(0);
     * protected_int.with_locked([](int& val){ val += 42; });
     * ```
     *
     * @param f The functor that will be called with a reference to the wrapped
     *          value while the <em>inner mutex</em> will be locked.
     */
    template<typename F>
    requires invokable_with<F, T&>
    decltype(auto) with_locked(F&& f) {
        notifier dn(*this);
        std::lock_guard lock(mtx_);
        return std::invoke(f, val_);
    }

    //! Gets a copy of the wrapped value while locking the inner mutex.
    //! If @a M is @link llh::mutexed::shared_lockable shared_lockable @endlink, `lock_shared()` will be used.
    template<typename = void>
    requires std::is_copy_constructible_v<T>
    T get_copy() const {
        possibly_shared_lock lock(mtx_);
        return val_;
    }


    /** @defgroup Waiting The waiting feature
     * The waiting feature is enabled if @a H is has_cv. It makes available the
     * three waiting functions that mirror the three waiting methods of
     * `std::condition_variable` and use them internally with a
     * possibly_shared_lock as first argument.
     *
     * Internally, the Mutexed will hold a @a condition-variable that will be
     * notified with `notify_all()` after the unlocking that occurs whenever the
     * <em>inner value</em> has been @a write-accessed, which happens at the end
     * of the calls to the non-`const` versions of locked() and with_locked().
     *
     * Here is an example of waiting on a Mutexed :
     * @code{.cpp}
     * struct future_int : std::optional<int> {
     *     void compute() { emplace(3); }
     * };
     * 
     * int main() {
     *     Mutexed<future_int, std::mutex, has_cv> init_after;
     * 
     *     // launching the thread that checks the result
     *     std::thread async_after_compute([&](){
     *         init_after.wait([](future_int const& fi){ return fi.has_value(); });
     *         std::cout << "The result is " << init_after.get_copy().value() << std::endl;
     *     });
     *     // making sure it stopped at the point where it waits
     *     std::this_thread::sleep_for(std::chrono::milliseconds(20));
     *
     *     // launching the thread that computes
     *     std::thread async_compute([&](){
     *         // change and notify
     *         init_after.with_locked(&future_int::compute);
     *     });
     *
     *     async_after_compute.join();
     *     async_compute.join();
     * }
     * @endcode
     *
     * @{
     */

    /** Waits until `this` is notified and the provided predicate returns `true`.
    *
    * Calling this function is blocking.
    *
    * The predicate is called with the <em>inner value</em> @a shared-locked if it
    * is @link llh::mutexed::shared_lockable shared_lockable @endlink, and
    * @a unique-locked otherwise.
    */
    template<typename Predicate>
    requires std::is_same_v<H, has_cv> && invokable_with<Predicate, T const&>
    void wait(Predicate&& p) const {
        possibly_shared_lock lock(mtx_);
        this->cv_.wait(lock, [p = std::forward<Predicate>(p), this](){ return std::invoke(p, val_); });
    }

    /** Waits until `this` is notified and the provided predicate returns
    *   `true` or until the specified duration has been spent waiting.
    *
    * @copydetails wait()
    */
    template<class Rep, class Period, typename Predicate>
    requires std::is_same_v<H, has_cv> && invokable_with<Predicate, T const&>
    bool wait_for(std::chrono::duration<Rep, Period> const& rel_time, Predicate&& p) const {
        possibly_shared_lock lock(mtx_);
        return this->cv_.wait_for(lock, rel_time, [p = std::forward<Predicate>(p), this](){ return std::invoke(p, val_); });
    }

    /** Waits until `this` is notified and the provided predicate returns
    *   `true` or until the specified time point has been passed.
    *
    * @copydetails wait()
    */
    template<class Clock, class Duration, typename Predicate>
    requires std::is_same_v<H, has_cv> && invokable_with<Predicate, T const&>
    bool wait_until(std::chrono::time_point<Clock, Duration> const& timeout_time, Predicate&& p) const {
        possibly_shared_lock lock(mtx_);
        return this->cv_.wait_until(lock, timeout_time, [p = std::forward<Predicate>(p), this](){ return std::invoke(p, val_); });
    }

    //! @}
    // end group Waiting


    /**
     *  @brief Provides access to the <i>wrapped value</i> through a tuple of an
     *  unspecified lock guard and a reference to the value.
     *
     *  Use it this way :
     *  ```cpp
     *  {
     *      auto [lock, ref] = protected.locked();
     *      ref += 42;
     *  }
     *  ```
     *
     *  This function <i>unique-locks</i> the <em>inner mutex</em> before
     *  returning the tuple. The lock-guard returned has a destructor that
     *  unlocks the <i>inner mutex</i> and then, if @ref Waiting is enabled,
     *  notifies the <i>inner condition-variable</i>.
     */
    decltype(auto) locked() {
        class Lock {
        private:
            Mutexed& m;

            void lock()   { m.mtx_.lock(); }
            void unlock() { m.mtx_.unlock(); }

        public:
            explicit Lock(Mutexed& mtx) : m(mtx) { lock(); }

            ~Lock() {
                unlock();
                if constexpr (std::is_same_v<H, has_cv>) {
                    m.cv_.notify_all();
                }
            }

            // Copies would mess with unlocks and notifications
            Lock(Lock const&) = delete;
            // Moves could have use-cases but would require tracking an otherwise useless state
            Lock(Lock &&) = delete;
        };
        return std::tuple<Lock, T&>(*this, val_);
    }
    //! Same as locked_const().
    std::tuple<possibly_shared_lock, T const&> locked() const {
        return locked_const();
    }
    /**
     *  @brief Provides `const` access to the <i>wrapped value</i> through a
     *  tuple of a @ref possibly_shared_lock and a `const` reference to the <i>
     *  wrapped value</i>.
     *
     *  Use it this way :
     *  ```cpp
     *  {
     *      auto const [lock, ref] = protected.locked_const();
     *      std::cout << ref;
     *  }
     *  ```
     *
     *  This function locks the <i>inner mutex</i> before returning the tuple.
     *  The locking is shared if that mutex is @link llh::mutexed::shared_lockable
     *  shared_lockable @endlink, and regular (`lock()` used) otherwise.
     *
     *  The lock guard returned has a destructor that unlocks the <i>inner mutex</i>.
     */
    std::tuple<possibly_shared_lock, T const&> locked_const() const {
        return std::tuple<possibly_shared_lock, T const&>{mtx_, val_};
    }
};


//! A value for the disambiguation tag type mutex_args_t provided as convenience.
inline constexpr mutex_args_t mutex_args{};
inline constexpr value_args_t value_args{};

//! A functor that locks in a deadlock-free way all the provided Mutexed.
inline constexpr details::all_locker with_all_locked{};

} // end namespace llh::mutexed
