# Mutexed
A header-only mutex-protected value wrapper for C++20.


# Rationale
Using mutexes on their own to protect data from concurrent access impose doing specific actions that may be forgotten by the user, making it error-prone :
```cpp
std::vector<std::string> users;
std::mutex users_mutex;

void handle_socket(/* ... */)
{
    /*
        very complicated things
        that may make you forget
        your duties
    */

    // OOPS ! forgot to lock
    users.push(user);

    // what should have happen
    {
        std::lock_guard<std::mutex> guard(users_mutex);
        users.push(user);
    }
}
```

The `Mutexed` class prevents the user from accessing the data without its dedicated mutex being locked.

# Full Documentation
The full documentation of this library is available at https://ll-h.github.io/mutexed.

# API Overview
Two ways of accessing the protected data are provided :

## with structured bindings
```cpp
llh::mutexed::Mutexed<std::vector<std::string>, std::mutex> protected_users;

void add_user(std::string_view user) {
    auto [lock, users] = protected_users.locked();
    users.push(user);
}
```

Note that the type of `users` is `std::vector<std::string>&`.

## using `with_locked`
```cpp
llh::mutexed::Mutexed<std::vector<std::string>, std::mutex> protected_users;

void add_user(std::string_view user) {
    protected_users.with_locked([](std::vector<std::string>& users) {
        users.push(user);
    });
}
```


# Shared-lockability
The `Mutexed` class detects if the mutex is `shared_lockable` (a concept that checks if it has the [`lock_shared()`](https://en.cppreference.com/w/cpp/thread/shared_mutex/lock_shared)-associated functions) and uses `lock_shared()` on each of the following circumstances :
* `lock_const()` is called
* `with_locked()` is called with a functor that accepts a `const&`
* the `Mutexed` is `const` when any of `with_locked()`, `locked()` or `when_all_locked()` is called

# Acquiring more than one `Mutexed`
>[!WARNING]
> * Subject to breaking changes because I think that having the function as last argument would be best but did not allocate enough time to find out how to do it
> * It does not notify the condition-variable yet

Acquiring more that one mutex is error-prone, that is why the standard library provides the free function [`std::lock()`](https://en.cppreference.com/w/cpp/thread/lock).

The `with_all_locked()` pseudo-free-function calls `std::lock()` under the hood and is used like this :
```cpp
llh::mutexed::with_all_locked([](auto& data_from_a, auto const& data_from_b) {
    /* use data_from_b to modify data_from_a */
    },
    mutexed_a,
    std::cref(mutexed_b)  // passing a const& will make it use lock_shared() when it exists
);
```


# Condition-variables
You may optionally have your `Mutexed` object hold a condition-variable by providing `llh::mutexed::has_cv` as its last template argument.

## Notifications
The non-`const` versions of `with_locked()` and `locked()` will call `notify_all()` on the condition-variable after the mutex have been unlocked.

## Waiting
The `Mutexed` class has the three member-functions
* `wait(Predicate&&)`
* `wait_for(std::chrono::duration const&, Predicate&&)`
* `wait_until(std::chrono::time_point const&, Predicate&&)`

that mirror the standard library's member functions of `std::condition_variable_any` called with a lock that is shared if the mutex is `shared_lockable`.


# Performance
The tests confirm that the number of times the inner mutex is acquired is exactly once for both of the ways to access the protected data.


# Compatibility
This library currently requires C++20, but it could be implemented in C++11 with a significant uglification of the code for the `with_locked()` API, going lower than that would make it prohibitively difficult to use due to the lack of lambdas. The `locked()` API requires C++17 for the structured-bindings and mendatory return value optimization that makes it possible to return a lock guard without acquiring the mutex more than once.


It has been tested in the following environments :
* Debian-based distribution with Clang-15 and libc++


# TODOs
* test on more environments
* copy and move constructors
* test `auto const` for the structured bindings
* installation guide
* make `with_all_locked` take the function as its last argument instead of first
* provide a structure bindings API for acquiring several `Mutexed`s
* constructor with 2 parameter packs to construct in place both the value and the mutex
* specialize for libcoro's coro::mutex
* make the code compatible with C++17 using `#ifdef`s
