#include "threadPool.h"

ThreadPool::ThreadPool(size_t threads) : stop(false), paused(false), activeThreads(0) {
    for(size_t i = 0; i < threads; ++i)
        workers.emplace_back(&ThreadPool::workerThread, this);
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::workerThread() {
    for(;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            condition.wait(lock, [this]{ return stop || (!paused && !tasks.empty()); });
            if(stop && tasks.empty())
                return;
            if (paused) {
                pauseCondition.wait(lock, [this]{ return !paused; });
                continue;
            }
            task = std::move(tasks.front());
            tasks.pop();
            ++activeThreads;
        }
        task();
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            --activeThreads;
            if (tasks.empty() && activeThreads == 0) {
                pauseCondition.notify_all();
            }
        }
    }
}

void ThreadPool::shutdown() {
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        stop = true;
    }
    condition.notify_all();
    for(std::thread &worker: workers)
        worker.join();
}

void ThreadPool::pause() {
    std::unique_lock<std::mutex> lock(queueMutex);
    paused = true;
}

void ThreadPool::resume() {
    {
        std::unique_lock<std::mutex> lock(queueMutex);
        paused = false;
    }
    pauseCondition.notify_all();
}

void ThreadPool::waitForCompletion() {
    std::unique_lock<std::mutex> lock(queueMutex);
    pauseCondition.wait(lock, [this]{ return tasks.empty() && activeThreads == 0; });
}

size_t ThreadPool::getQueueSize() {
    std::unique_lock<std::mutex> lock(queueMutex);
    return tasks.size();
}

size_t ThreadPool::getActiveThreads() {
    std::unique_lock<std::mutex> lock(queueMutex);
    return activeThreads.load();
}

void ThreadPool::resize(size_t newSize) {
    std::unique_lock<std::mutex> lock(queueMutex);
    size_t currentSize = workers.size();
    if (newSize > currentSize) {
        for (size_t i = currentSize; i < newSize; ++i) {
            addWorker();
        }
    } else if (newSize < currentSize) {
        size_t diff = currentSize - newSize;
        for (size_t i = 0; i < diff; ++i) {
            enqueue([this] { stop = true; }); // Use a flag to indicate the worker should terminate
        }
    }
}

void ThreadPool::addWorker() {
    workers.emplace_back(&ThreadPool::workerThread, this);
}

size_t ThreadPool::getSize() {
    std::unique_lock<std::mutex> lock(queueMutex);
    return workers.size();
}
