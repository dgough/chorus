#pragma once

#ifndef CHORUS_OBJECT_POOl_H
#define CHORUS_OBJECT_POOl_H

#include <deque>
#include <memory>
#include <thread>
#include <mutex>
#include <future>
#include <atomic>
#include <algorithm>
#include <cassert>

// This macro can be used to specify an alternative condition variable class
// as long as it has the same signature as std::condition_variable.
#ifndef CONDITION_VARIABLE
#include <condition_variable>
#define CONDITION_VARIABLE std::condition_variable
#endif // !CONDITION_VARIABLE

namespace chorus {

// This class exists because SonarQube hates forward declarations
template<typename T>
class IObjectPool {
public:
    virtual ~IObjectPool() {}
    virtual void add(std::unique_ptr<T> object) = 0;
};

/**
 * PoolHandle is a smart pointer that manages an object through a pointer and puts that object
 * back in the linked ObjectPool when the PoolHandle is destroyed.
 * If the ObjectPool was destroyed then the managed object is destroyed.
 * 
 * Unlike other smart pointers, this class does not support release().
 */
template<typename T>
class PoolHandle final {
public:
    /**
     * Creates an empty PoolHandle.
     */
    PoolHandle() = default;

    /**
     * Creates a PoolHandle an assigns it a unique_ptr to manage.
     * @param[in] ptr  The unique_ptr that this handle manages.
     * @param[in] pool The object pool that this handle will add the object to.
     */
    PoolHandle(std::unique_ptr<T> ptr, std::shared_ptr<IObjectPool<T>> pool) 
        : m_ptr(std::move(ptr)), m_pool(pool) {}
    /**
     * The destructor will put the object back into the pool.
     * If the pool was destroyed then the object will also be destroyed.
     */
    ~PoolHandle();

    // Not copyable
    PoolHandle(const PoolHandle&) = delete;
    PoolHandle& operator=(const PoolHandle&) = delete;

    // Movable
    PoolHandle(PoolHandle&&) = default;
    PoolHandle& operator=(PoolHandle&&) = default;

    PoolHandle(std::nullptr_t) noexcept {}
    PoolHandle& operator=(std::nullptr_t);

    /**
     * Returns a pointer to the managed object.
     */
    T* get() const noexcept {
        return m_ptr.get();
    }

    typename std::add_lvalue_reference<T>::type operator*() const {
        return *m_ptr;
    }

    T* operator->() const noexcept {
        return m_ptr.get();
    }

    /**
     * Returns true if this PoolHandle is managing an object; false otherwise.
     */
    explicit operator bool() const noexcept {
        return static_cast<bool>(m_ptr);
    }

    /**
     * Resets the PoolHandle and releases ownership of the object.
     * Similar to the destructor, if this handle is linked to a pool then the object will be added
     * back to the pool otherwise the object will be destroyed.
     */
    void reset();
private:
    /**
     * Returns the managed object to the pool if it still exists.
     */
    void returnObject();

    std::unique_ptr<T> m_ptr;
    std::weak_ptr<IObjectPool<T>> m_pool;
};

/**
 * ObjectPool is a thread safe pool of objects that can be borrowed.
 * If there are no objects available to be borrowed then the requesting threads 
 * will be places in a FIFO queue.
 */
template<typename T>
class ObjectPool : public IObjectPool<T>, public std::enable_shared_from_this<ObjectPool<T>> {
public:
    using SharedPtr = std::shared_ptr<ObjectPool<T>>;

    // This class must be instantiated with a shared_ptr. Use the static create method instead.
    ObjectPool() {}
    virtual ~ObjectPool();

    // Not copyable and not movable
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    /**
     * Creates an empty ObjectPool.
     */
    static ObjectPool::SharedPtr create();

    /**
     * Borrows one of the objects from the pool.
     * Returns a handle to the borrowed object.
     * The borrowed object is returned to the pool when the handle is destroyed.
     */
    PoolHandle<T> borrow();

    /**
     * Similar to borrow() except if waitDuration expires then an empty PoolHandle is returned.
     * Passing a negative duration is the same as passing zero (meaning don't wait).
     */
    template<typename Rep, typename Period>
    PoolHandle<T> borrow(const std::chrono::duration<Rep, Period>& waitDuration);

    /**
     * Similar to borrow() except if timeoutTime expires then an empty PoolHandle is returned.
     */
    template<class Clock, class Duration>
    PoolHandle<T> borrow(const std::chrono::time_point<Clock, Duration>& timeoutTime);

    /**
     * Adds an object to the pool.
     * This object will be owned by the pool and available to be borrowed.
     */
    void add(std::unique_ptr<T> object) override;

private:
    struct WaitingThread {
        WaitingThread() : id(std::this_thread::get_id()) {}

        std::thread::id id;
        std::promise<void> promise;
    };

    /**
     * Returns true if an object is available in the pool.
     * Only call this method while the mutex is locked.
     */
    inline bool objectAvailable() const;

    /**
     * Returns a handle to the next object.
     * This method must be called while the mutex is locked by this thread
     * and while the objects collection is not empty.
     */
    PoolHandle<T> getNextHandle();

    /**
     * Removes the calling thread from the waiting queue.
     */
    void removeSelfFromQueue();

    std::deque<std::unique_ptr<T>> m_objects;
    std::deque<WaitingThread> m_waitingQueue;
    CONDITION_VARIABLE m_cond;
    mutable std::mutex m_mutex;
};

// PoolHandle<T>

template<typename T>
PoolHandle<T>::~PoolHandle() {
    returnObject();
}

template<typename T>
PoolHandle<T>& PoolHandle<T>::operator=(nullptr_t) {
    reset();
    return *this;
}

template<typename T>
void PoolHandle<T>::returnObject() {
    if (auto pool = m_pool.lock()) {
        pool->add(std::move(m_ptr));
    }
}

template<typename T>
void PoolHandle<T>::reset() {
    returnObject();
    m_ptr.reset();
    m_pool.reset();
}

// ObjectPool<T>

template<typename T>
ObjectPool<T>::~ObjectPool() {
    // TODO: log error if m_waitingQueue is not empty
}

template<typename T>
typename ObjectPool<T>::SharedPtr ObjectPool<T>::create() {
    return std::make_shared<ObjectPool<T>>(); 
}

template<typename T>
PoolHandle<T> ObjectPool<T>::borrow() {
    std::unique_lock<std::mutex> lk(m_mutex);
    
    // if the object is available then no need to queue
    if (objectAvailable()) {
        return getNextHandle();
    }

    // join the waiting queue
    m_waitingQueue.emplace_back();
    auto future = m_waitingQueue.back().promise.get_future();
    lk.unlock();

    // wait until it is this thread's turn
    future.get();
    lk.lock();
    m_cond.wait(lk, [this] {
        return objectAvailable();
    });
    return getNextHandle();
}

template<typename T>
template<typename Rep, typename Period>
PoolHandle<T> ObjectPool<T>::borrow(const std::chrono::duration<Rep, Period>& waitDuration) {
    // on Windows time_point::max() wasn't working and anything over 2135819 hours also wasn't working.
    // 2 million hours (~228 years) should be a long enough wait time.
    static constexpr auto MaxTimePoint = std::chrono::time_point<std::chrono::steady_clock>(std::chrono::hours(2000000));
    std::chrono::steady_clock::time_point timePoint;
    auto now = std::chrono::steady_clock::now();
    if (waitDuration < std::chrono::duration<Rep, Period>::zero()) {
        timePoint = now;
    }
    else if (now > MaxTimePoint - waitDuration) {
        // would have overflowed
        timePoint = MaxTimePoint;
    }
    else {
        timePoint = now + waitDuration;
    }
    return borrow(timePoint);
}

template<typename T>
template<class Clock, class Duration>
PoolHandle<T> ObjectPool<T>::borrow(const std::chrono::time_point<Clock, Duration>& timeoutTime) {
    std::unique_lock<std::mutex> lk(m_mutex);

    // if the object is available then no need to queue
    if (objectAvailable()) {
        return getNextHandle();
    }

    // don't bother joining the queue if the time point isn't a future time point
    if (timeoutTime <= std::chrono::steady_clock::now()) {
        return nullptr;
    }

    // join the waiting queue
    m_waitingQueue.emplace_back();
    auto future = m_waitingQueue.back().promise.get_future();
    lk.unlock();

    // wait until it is this thread's turn
    if (future.wait_until(timeoutTime) == std::future_status::timeout) {
        removeSelfFromQueue();
        return nullptr;
    }
    future.get();

    lk.lock();
    bool available = m_cond.wait_until(lk, timeoutTime, [this] {
        return objectAvailable();
    });
    if (available) {
        return getNextHandle();
    }
    removeSelfFromQueue();
    return nullptr;
}

template<typename T>
void ObjectPool<T>::add(std::unique_ptr<T> object) {
    if (object == nullptr) {
        return;
    }
    std::unique_lock<std::mutex> lk(m_mutex);
    m_objects.push_back(std::move(object));
    if (!m_waitingQueue.empty()) {
        m_waitingQueue.front().promise.set_value();
        m_waitingQueue.pop_front();
    }
    lk.unlock();
    m_cond.notify_one();
}

template<typename T>
inline bool ObjectPool<T>::objectAvailable() const {
    // assumes mutex is already locked
    return !m_objects.empty();
}

template<typename T>
PoolHandle<T> ObjectPool<T>::getNextHandle() {
    // assumes mutex is already locked
    assert(objectAvailable());
    PoolHandle<T> handle(std::move(m_objects.front()), this->shared_from_this());
    m_objects.pop_front();
    return handle;
}

template<typename T>
void ObjectPool<T>::removeSelfFromQueue() {
    // assumes mutex is already locked
    auto id = std::this_thread::get_id();
    auto it = std::find_if(m_waitingQueue.begin(), m_waitingQueue.end(), [id](const WaitingThread& w) {
        return w.id == id;
    });
    if (it != m_waitingQueue.end()) {
        m_waitingQueue.erase(it);
    }
}

// equality operators

template<typename T>
bool operator==(const PoolHandle<T>& handle, std::nullptr_t) noexcept {
    return handle.get() == nullptr;
}

template<typename T>
bool operator==(std::nullptr_t, const PoolHandle<T>& handle) noexcept {
    return handle.get() == nullptr;
}

template<typename T>
bool operator!=(const PoolHandle<T>& handle, std::nullptr_t) noexcept {
    return !(handle == nullptr);
}

template<typename T>
bool operator!=(std::nullptr_t, const PoolHandle<T>& handle) noexcept {
    return !(handle == nullptr);
}

} // namespace chorus

#endif CHORUS_OBJECT_POOl_H
