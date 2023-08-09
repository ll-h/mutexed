#include <condition_variable>
#include <shared_mutex>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <functional>

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

inline struct all_locker {
    template<typename M>
    struct lockable_proxy {
        M& m;

        void lock() { m.lock(); }
        void unlock() { m.unlock(); }
        bool try_lock() { return m.try_lock(); }
    };

    template<typename M>
    requires shared_lockable<M>
    struct lockable_proxy<M const> {
    private:
        M& m;
    public:
        
        explicit lockable_proxy(M const& c_m) : m(const_cast<M&>(c_m)) {}

        void lock() { m.lock_shared(); }
        void unlock() { m.unlock_shared(); }
        bool try_lock() { return m.try_lock_shared(); }
    };

    template<typename F, typename... M>
    requires std::conjunction_v<std::is_base_of<mutexed_tag, std::decay_t<M>>...>
    decltype(auto) operator()(F&& f, M&... mtxs) const {
        return [&](lockable_proxy<M>&&... m) {
            std::scoped_lock<lockable_proxy<M>...> lock(m...);
            return std::invoke(std::forward<F>(f), mtxs.val_...);
        }(lockable_proxy<M>{mtxs}...);
    }
} with_unlocked{};


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
    
    template<typename Self>
    static decltype(auto) as_shared_if_const(Self& self) {
        if constexpr (shared_lockable<M> && std::is_same_v<Self, Self const>) {
            return self.as_wrapper();
        }
        else {
            return self.mtx_;
        }
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
    
public:
    using value_type = T;

    Mutexed(Mutexed&&) = delete;
    Mutexed(Mutexed const&) = delete;
    
    template<typename... Args>
    explicit Mutexed(Args... args) : val_(std::forward<Args>(args)...) {}

    template<typename F>
    requires
        invokable_with<F, T const&> ||
        invokable_with<F, T> && std::is_copy_constructible_v<T>
    decltype(auto) with_unlocked(F&& f) const {
        std::shared_lock lock(as_wrapper());
        return std::invoke(std::forward<F>(f), std::cref(val_).get());
    }

    template<typename F>
    requires invokable_with<F, T&>
    decltype(auto) with_unlocked(F&& f) {
        notifier dn(*this);
        std::lock_guard<M> lock(this->mtx_);
        return std::invoke(f, std::ref(val_).get());
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
        std::unique_lock<M> lock(this->mtx_);
        this->cv_.wait(lock, [p = std::forward<Predicate>(p), this](){ return std::invoke(p, val_); });
    }

    template<typename F>
    requires std::is_same_v<H, has_cv> && invokable_with<F, T&>
    decltype(auto) with_unlocked_notify(F&& f) {
        return with_unlocked(std::forward<F>(f));
    }
};