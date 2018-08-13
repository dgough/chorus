#pragma once

#ifndef CHORUS_OBJECT_POOl_H
#define CHORUS_OBJECT_POOl_H

#include <mutex>
#include <deque>
#include <future>
#include <thread>
#include <memory>
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
class PoolHandle {
public:
    /**
     * Creates an empty PoolHandle.
     */
    PoolHandle() {};

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
    virtual ~PoolHandle();

    // Not copyable
    PoolHandle(const PoolHandle&) = delete;
    PoolHandle& operator=(const PoolHandle&) = delete;

    // Movable
    PoolHandle(PoolHandle&&) = default;
    PoolHandle& operator=(PoolHandle&&) = default;

    T* get() const noexcept {
        return m_ptr.get();
    }

    typename std::add_lvalue_reference<T>::type operator*() const {
        return *m_ptr;
    }

    T* operator->() const noexcept {
        return m_ptr.get();
    }

    explicit operator bool() const noexcept {
        return static_cast<bool>(m_ptr);
    }

    void reset() noexcept {
        m_ptr.reset();
        m_pool.reset();
    }
private:
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
     * Returns a handle to the borrowed object. The object is returned to the pool when the handle is destroyed.
     */
    PoolHandle<T> borrow();

    /**
     * Adds an object to the pool.
     * This object will be owned by the pool and available to be borrowed.
     */
    void add(std::unique_ptr<T> object) override;

private:

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

    std::deque<std::unique_ptr<T>> m_objects;
    std::deque<std::promise<void>> m_waitingQueue;
    CONDITION_VARIABLE m_cond;
    mutable std::mutex m_mutex;
};

// PoolHandle<T>

template<typename T>
PoolHandle<T>::~PoolHandle() {
    if (auto pool = m_pool.lock()) {
        pool->add(std::move(m_ptr));
    }
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
typename PoolHandle<T> ObjectPool<T>::borrow() {
    std::unique_lock<std::mutex> lk(m_mutex);
    
    // if the object is available then no need to queue
    if (objectAvailable()) {
        return getNextHandle();
    }

    // join the waiting queue
    m_waitingQueue.emplace_back();
    auto future = m_waitingQueue.back().get_future();
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
void ObjectPool<T>::add(std::unique_ptr<T> object) {
    if (object == nullptr) {
        return;
    }
    std::unique_lock<std::mutex> lk(m_mutex);
    m_objects.push_back(std::move(object));
    if (!m_waitingQueue.empty()) {
        m_waitingQueue.front().set_value();
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

// equality operators

template<typename T>
bool operator==(const PoolHandle<T>& handle, nullptr_t) noexcept {
    return handle.get() == nullptr;
}

template<typename T>
bool operator==(nullptr_t, const PoolHandle<T>& handle) noexcept {
    return handle.get() == nullptr;
}

template<typename T>
bool operator!=(const PoolHandle<T>& handle, nullptr_t) noexcept {
    return !(handle == nullptr);
}

template<typename T>
bool operator!=(nullptr_t, const PoolHandle<T>& handle) noexcept {
    return !(handle == nullptr);
}

} // namespace chorus

#endif CHORUS_OBJECT_POOl_H
