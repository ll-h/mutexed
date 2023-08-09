#include <memory>
#define BOOST_TEST_MODULE Mutexed
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>
#include <iostream>
#include <shared_mutex>
#include <type_traits>
#include <utility>
#include <functional>

#include <thread>
#include <chrono>

#include "mutexed.hpp"


struct instrumented_shared_mutex : std::shared_mutex {
private:
    static bool* has_been_shared_locked_;

    std::shared_mutex& as_parent() { return *this; }

public:
    static bool has_been_shared_locked() { return *has_been_shared_locked_; }
    static void set_flag_ref(std::reference_wrapper<bool> ref) { has_been_shared_locked_ = std::addressof(ref.get()); }
    
    using std::shared_mutex::shared_mutex;

    void lock_shared() {
        *has_been_shared_locked_ = true;
        as_parent().lock_shared();
    }
    bool try_lock_shared() {
        *has_been_shared_locked_ = true;
        return as_parent().try_lock_shared();
    }
};

bool* instrumented_shared_mutex::has_been_shared_locked_ = nullptr;


BOOST_AUTO_TEST_SUITE(MutexedTests)

BOOST_AUTO_TEST_CASE(Mutexed_GetCopy)
{
    Mutexed<int> const mutexed(42);
    int copy = mutexed.get_copy();
    BOOST_TEST(copy == 42);
}

BOOST_AUTO_TEST_CASE(Mutexed_WithUnlocked_Const)
{
    Mutexed<int> const mutexed(42);
    int result = mutexed.with_unlocked([](const int& value) {
        BOOST_TEST(value == 42);
        return value * 2;
    });
    BOOST_TEST(result == 84);
}

BOOST_AUTO_TEST_CASE(Mutexed_WithUnlocked_Mut)
{
    Mutexed<int> mutexed(42);
    mutexed.with_unlocked([](int& value) {
        value += 10;
        return value;
    });
    BOOST_TEST(mutexed.get_copy() == 52);
}

BOOST_AUTO_TEST_CASE(Multi_WithUnlocked)
{
    bool has_been_shared_locked = false;
    instrumented_shared_mutex::set_flag_ref(has_been_shared_locked);
    Mutexed<int, instrumented_shared_mutex> const c_mut(42);
    Mutexed<int>                                  mut_mut(8);
    
    int extracted_from_const = 0;
    
    with_unlocked([&extracted_from_const](int c, int& m) {
            extracted_from_const = c;
            m = 10;
        },
        c_mut, mut_mut
    );

    BOOST_TEST(mut_mut.get_copy() == 10);
    BOOST_TEST(extracted_from_const == 42);
    BOOST_TEST(has_been_shared_locked == true);

    // TODO test shared non const
}

BOOST_AUTO_TEST_SUITE_END()


// Helper function to increment a Mutexed<int> value in a loop
void incrementValue(Mutexed<int>& mutexed, int iterations) {
    for (int i = 0; i < iterations; ++i) {
        mutexed.with_unlocked([](int& value) {
            ++value;
        });
    }
}

BOOST_AUTO_TEST_SUITE(MutexedThreadSafetyTests)

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

template<typename M>
void test_sync() {
    Mutexed<flagged_int, M, has_cv> init_after;

    // launching the thread that should waits
    std::thread to_do_after([&](){
        init_after.wait([](flagged_int const& fi){ return fi.initialized; });
        init_after.with_unlocked([](flagged_int& fi){ fi.val *= 3; });
    });
    // making sure it stopped at the point where it waits
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    
    // change and notify
    init_after.with_unlocked_notify([](flagged_int& fi){ fi.set(2); });

    to_do_after.join();

    BOOST_TEST(init_after.get_copy().val == 6);
}

BOOST_AUTO_TEST_CASE(stdMutex_CV_sync)
{
    test_sync<std::mutex>();
}
BOOST_AUTO_TEST_CASE(stdSharedMutex_CV_sync)
{
    test_sync<std::shared_mutex>();
}

BOOST_AUTO_TEST_SUITE_END()
