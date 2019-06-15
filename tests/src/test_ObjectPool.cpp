#include <gtest/gtest.h>
#include <chorus/ObjectPool.hpp>
#include <string>
#include <future>
#include <vector>

namespace chorus {

using std::string;
using std::chrono::steady_clock;
using std::chrono::seconds;
using std::chrono::milliseconds;
using std::chrono::nanoseconds;

TEST(ObjectPool, emptyHandle) {
    PoolHandle<string> handle;
    ASSERT_EQ(nullptr, handle);
    ASSERT_EQ(handle, nullptr);
    ASSERT_EQ(nullptr, handle.get());
}

TEST(ObjectPool, basic) {
    const char * const str = "first";
    auto pool = ObjectPool<string>::create();
    ASSERT_NE(nullptr, pool);
    pool->add(std::make_unique<string>(str));

    auto handle = pool->borrow();
    ASSERT_NE(nullptr, handle);
    ASSERT_NE(handle, nullptr);
    EXPECT_STREQ(str, handle->c_str());
}

TEST(ObjectPool, nullptrHandle) {
    PoolHandle<string> handle(nullptr);
    EXPECT_EQ(nullptr, handle);

    auto pool = ObjectPool<string>::create();
    pool->add(std::make_unique<string>("str"));
    handle = pool->borrow();
    EXPECT_NE(nullptr, handle);
    EXPECT_NE(nullptr, handle.get());
    EXPECT_STREQ("str", handle->c_str());
    handle = nullptr;
    EXPECT_EQ(nullptr, handle);
    auto handle2 = pool->borrow();
    EXPECT_NE(nullptr, handle2);
    handle = std::move(handle2);
    EXPECT_EQ(nullptr, handle2);
    EXPECT_NE(nullptr, handle);
}

TEST(ObjectPool, order) {
    // test to make sure that the threads that borrow the object get it in the correct order
    auto pool = ObjectPool<string>::create();
    const std::vector<char> letters = {'A','B','C','D'};
    auto str = std::make_unique<string>();
    str->push_back(letters[0]);

    auto f = [pool](char expectedChar, std::shared_ptr<std::promise<void>> promise) {
        promise->set_value();
        auto handle = pool->borrow();
        ASSERT_NE(nullptr, handle);
        EXPECT_EQ(expectedChar, handle->at(0));
        handle->operator[](0) += 1;
        std::this_thread::sleep_for(milliseconds(1));
    };

    std::vector<std::future<void>> asyncResults;

    for (char letter : letters) {
        auto promise = std::make_shared<std::promise<void>>();
        auto threadStarted = promise->get_future();
        asyncResults.push_back(std::async(std::launch::async, f, letter, promise));
        threadStarted.get();
    }

    // Add the object to the pool
    pool->add(std::move(str));

    // wait for all asyncs to complete
    for (auto& future : asyncResults) {
        future.get();
    }

    auto handle = pool->borrow();
    EXPECT_EQ('E', handle->at(0));
}

TEST(ObjectPool, destroyPool) {
    // tests that destroying the pool before the handle results in 
    // the object not being destroyed until the handle is destroyed.
    static std::atomic_bool itemDestroyed{false};
    struct Item final {
        ~Item() {
            itemDestroyed = true;
        }
    };
    EXPECT_FALSE(itemDestroyed.load());
    auto pool = ObjectPool<Item>::create();
    pool->add(std::make_unique<Item>());
    auto handle = pool->borrow();
    pool.reset(); // destroy the pool
    EXPECT_FALSE(itemDestroyed.load());
    handle.reset();
    EXPECT_TRUE(itemDestroyed.load());
}

TEST(ObjectPool, waitRelativeTime) {
    // waits a relative amount of time and ensures that time has passed
    auto pool = ObjectPool<string>::create();
    constexpr auto waitTime = milliseconds(50);
    auto start = steady_clock::now();
    auto handle = pool->borrow(waitTime);
    EXPECT_EQ(nullptr, handle);
    auto delta = steady_clock::now() - start;
    EXPECT_GE(delta, waitTime);
}

TEST(ObjectPool, waitMaxDuration) {
    // Tests waiting to borrow for max duration.
    // Make sure there isn't an overflow bug that results in returning immediately.
    auto pool = ObjectPool<string>::create();
    static constexpr auto waitTime = milliseconds(60);
    auto start = steady_clock::now();
    auto thread = std::thread([pool] {
        std::this_thread::sleep_for(waitTime);
        pool->add(std::make_unique<string>());
    });
    auto handle = pool->borrow(nanoseconds::max());
    EXPECT_NE(nullptr, handle);
    auto delta = steady_clock::now() - start;
    EXPECT_GE(delta, waitTime);
    thread.join();
}

TEST(ObjectPool, negativeDuration) {
    // Negative durations shouldn't wait
    auto pool = ObjectPool<string>::create();
    auto handle = pool->borrow(seconds(-30));
    EXPECT_EQ(nullptr, handle);

    pool->add(std::make_unique<string>("stuff"));
    handle = pool->borrow(seconds(-30));
    ASSERT_NE(nullptr, handle);
    EXPECT_EQ(*handle, "stuff");
}

TEST(ObjectPool, waitTimePoint) {
    // waits until a specific time point and ensures that time has passed
    auto pool = ObjectPool<string>::create();
    constexpr auto waitTime = milliseconds(50);
    auto start = steady_clock::now();
    auto handle = pool->borrow(steady_clock::now() + waitTime);
    EXPECT_EQ(nullptr, handle);
    auto delta = steady_clock::now() - start;
    EXPECT_GE(delta, waitTime);
}

TEST(ObjectPool, resetHandle) {
    auto pool = ObjectPool<string>::create();
    pool->add(std::make_unique<string>("stuff"));
    auto handle = pool->borrow();
    ASSERT_NE(nullptr, handle);
    handle.reset();
    EXPECT_EQ(nullptr, handle);

    // the object should have been put back into the pool
    handle = pool->borrow(steady_clock::now());
    ASSERT_NE(nullptr, handle);
}

TEST(ObjectPool, removeFromQueue) {
    // tests that a thread is removed from waiting queue if it stops waiting
    auto pool = ObjectPool<string>::create();

    auto t1 = std::thread([pool] {
        auto handle = pool->borrow(milliseconds(5));
        EXPECT_EQ(nullptr, handle);
    });
    t1.join();

    constexpr auto maxWaitTime = seconds(5);
    auto t2 = std::thread([pool] {
        std::this_thread::sleep_for(milliseconds(30));
        pool->add(std::make_unique<string>("word"));
    });
    auto start = steady_clock::now();
    auto handle = pool->borrow(maxWaitTime);
    EXPECT_NE(nullptr, handle);
    EXPECT_LT(steady_clock::now() - start, maxWaitTime);
    t2.join();
}

} // namespace chorus
