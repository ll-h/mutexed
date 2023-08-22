#define BOOST_TEST_MODULE Mutexed
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>
#include <iostream>
#include <shared_mutex>
#include <type_traits>
#include <utility>
#include <functional>
#include <optional>

#include <thread>
#include <chrono>

#include "mutexed.hpp"

using namespace llh::mutexed;


BOOST_AUTO_TEST_SUITE(APITests)

struct lock_stats {
    unsigned int nb_locked = 0;
    unsigned int nb_try_locked = 0;
    unsigned int nb_unlocked = 0;
    unsigned int nb_locked_shared = 0;
    unsigned int nb_try_locked_shared = 0;
    unsigned int nb_unlocked_shared = 0;

    bool has_been_shared_locked() const {
        return nb_locked_shared > 0 || nb_try_locked_shared > 0;
    }
    bool has_been_unique_locked() const {
        return nb_locked > 0 || nb_try_locked > 0;
    }
};

template<typename M>
struct only_lockable_spy {
    lock_stats& stats;
    M mtx;

    only_lockable_spy(std::reference_wrapper<lock_stats> ls) : stats(ls), mtx() {}

    void lock() {
        mtx.lock();
        ++stats.nb_locked;
    }
    void unlock() {
        mtx.unlock();
        ++stats.nb_unlocked;
    }
    bool try_lock() {
        ++stats.nb_try_locked;
        return mtx.try_lock();
    }
};

template<typename M>
struct lockable_spy : only_lockable_spy<M> {
    using only_lockable_spy<M>::only_lockable_spy;
};

template<typename M>
requires shared_lockable<M>
struct lockable_spy<M> : only_lockable_spy<M> {
    using only_lockable_spy<M>::only_lockable_spy;

    void lock_shared() {
        this->mtx.lock_shared();
        ++this->stats.nb_locked_shared;
    }
    void unlock_shared() {
        this->mtx.unlock_shared();
        ++this->stats.nb_unlocked_shared;
    }
    bool try_lock_shared() {
        ++this->stats.nb_try_locked_shared;
        return this->mtx.try_lock_shared();
    }
};


BOOST_AUTO_TEST_CASE(Mutexed_GetCopy)
{
    Mutexed<int> const mutexed(42);
    int copy = mutexed.get_copy();
    BOOST_TEST(copy == 42);
}

BOOST_AUTO_TEST_CASE(Mutexed_WithLocked_Const)
{
    Mutexed<int> const mutexed(42);
    int result = mutexed.with_locked([](const int& value) {
        BOOST_TEST(value == 42);
        return value * 2;
    });
    BOOST_TEST(result == 84);
}

BOOST_AUTO_TEST_CASE(Mutexed_WithLocked_Mut)
{
    lock_stats stats;
    Mutexed<int, lockable_spy<std::shared_mutex>> mutexed(42, stats);
    mutexed.with_locked([&stats](int& value) {
        BOOST_TEST(value == 42);
        BOOST_TEST(stats.nb_locked == 1);
        value += 10;
        return value;
    });
    BOOST_TEST(stats.nb_unlocked == 1);

    BOOST_TEST(stats.nb_try_locked == 0);
    BOOST_TEST(stats.nb_locked_shared == 0);
    BOOST_TEST(stats.nb_unlocked_shared == 0);
    BOOST_TEST(stats.nb_try_locked_shared == 0);
    BOOST_TEST(mutexed.get_copy() == 52);
}


template<typename CallLocked>
void test_locked_const(CallLocked&& call_locked)
{
    lock_stats stats;
    Mutexed<int, lockable_spy<std::shared_mutex>> mutexed(42, stats);
    {
        auto [lock, value] = call_locked(mutexed);
        static_assert(std::is_same_v<decltype(value), int const&>);
        BOOST_TEST(value == 42);

        // making sure that the lock was acquired only once
        BOOST_TEST(stats.nb_locked_shared == 1);
    }
    BOOST_TEST(stats.nb_unlocked_shared == 1);

    BOOST_TEST(stats.nb_try_locked_shared == 0);
    BOOST_TEST(stats.nb_locked == 0);
    BOOST_TEST(stats.nb_unlocked == 0);
    BOOST_TEST(stats.nb_try_locked == 0);
}
BOOST_AUTO_TEST_CASE(Mutexed_Locked_Const)
{
    test_locked_const([](auto& mutexed){
        return mutexed.locked_const();
    });
}
BOOST_AUTO_TEST_CASE(Const_Mutexed_Locked)
{
    test_locked_const([](auto const& mutexed){
        return mutexed.locked();
    });
}


BOOST_AUTO_TEST_CASE(Mutexed_Locked_Mut)
{
    lock_stats stats;
    Mutexed<int, lockable_spy<std::shared_mutex>> mutexed(42, stats);
    {
        auto [lock, value] = mutexed.locked();
        static_assert(std::is_same_v<decltype(value), int &>);
        BOOST_TEST(value == 42);
        BOOST_TEST(stats.nb_locked == 1);

        value += 10;
    }
    BOOST_TEST(stats.nb_unlocked == 1);

    BOOST_TEST(stats.nb_try_locked == 0);
    BOOST_TEST(stats.nb_locked_shared == 0);
    BOOST_TEST(stats.nb_unlocked_shared == 0);
    BOOST_TEST(stats.nb_try_locked_shared == 0);

    BOOST_TEST(mutexed.get_copy() == 52);
    BOOST_TEST(stats.nb_locked_shared == 1);
    BOOST_TEST(stats.nb_unlocked_shared == 1);
}


BOOST_AUTO_TEST_CASE(WithAllLocked)
{
    lock_stats stats;
    Mutexed<int, lockable_spy<std::shared_mutex>> a(42, stats);
    Mutexed<int> b(8);

    int from_a = with_all_locked([](int in_a, int& in_b) {
            in_b = 10;
	    return in_a;
        },
        // pass a const& or a std::reference_wrapper<const Mutexed> to make it use lock_shared()
        std::cref(a), b
    );

    BOOST_TEST(stats.has_been_shared_locked() == true);
    BOOST_TEST(stats.has_been_unique_locked() == false);
    BOOST_TEST(b.get_copy() == 10);
    BOOST_TEST(from_a == 42);

    // reset stats
    stats = lock_stats();

    // testing if the mutex a is uniquely locked
    with_all_locked([](int&, int&) {}, a, b);
    BOOST_TEST(stats.has_been_shared_locked() == false);
    BOOST_TEST(stats.has_been_unique_locked() == true);
}

BOOST_AUTO_TEST_SUITE_END()


// Helper function to increment a Mutexed<int> value in a loop
void incrementValue(Mutexed<int>& mutexed, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        mutexed.with_locked([](int& value) {
            ++value;
        });
    }
}

BOOST_AUTO_TEST_SUITE(ThreadSafetyTests)

BOOST_AUTO_TEST_CASE(ConcurrentAccess)
{
    const int numThreads = 16;
    const int iterations = 1000;

    Mutexed<int> mutexed(0);

    std::vector<std::thread> threads;

    // Create multiple threads that concurrently increment the Mutexed<int> value
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back(incrementValue, std::ref(mutexed), iterations);
    }

    // Wait for all threads to finish
    for (auto& thread : threads) {
        thread.join();
    }

    int expectedValue = numThreads * iterations;

    BOOST_TEST(mutexed.get_copy() == expectedValue);
}

struct flagged_int {
    int val = 1;
    bool initialized = false;

    void set(int v) {
        val = v;
        initialized = true;
    }

    bool was_initialized() const { return initialized; }
};

struct future_int : std::optional<int> {
    void compute() { emplace(3); }
};

template<typename M>
void test_sync() {
    Mutexed<future_int, M, has_cv> init_after;

    // launching the thread that checks the result
    bool waiting_is_over = false;
    std::thread async_after_compute([&](){
        init_after.wait([](future_int const& fi){ return fi.has_value(); });
        BOOST_TEST(init_after.get_copy().value() == 3);
        waiting_is_over = true;
    });
    // making sure it stopped at the point where it waits
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // launching the thread that computes
    std::thread async_compute([&](){
        // change and notify
        init_after.with_locked(&future_int::compute);
    });

    async_after_compute.join();
    async_compute.join();

    BOOST_TEST(waiting_is_over);
}

BOOST_AUTO_TEST_CASE(stdMutex_CV_sync)
{
    test_sync<std::mutex>();
}
BOOST_AUTO_TEST_CASE(stdSharedMutex_CV_sync)
{
    test_sync<std::shared_mutex>();
}

BOOST_AUTO_TEST_CASE(stdMutex_CV_sync_from_locked)
{
    Mutexed<flagged_int, std::mutex, has_cv> init_after;

    // launching the thread that should wait
    std::thread to_do_after([&](){
        init_after.wait([](flagged_int const& fi){ return fi.initialized; });
        auto [lock, fi] = init_after.locked();
        fi.val *= 3;
    });
    // making sure it stopped at the point where it waits
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // change and notify
    {
        auto [lock, fi] = init_after.locked();
        fi.set(2);
    }

    to_do_after.join();

    BOOST_TEST(init_after.get_copy().val == 6);
}

BOOST_AUTO_TEST_SUITE_END()
