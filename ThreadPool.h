#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <memory>
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <future>
#include <condition_variable>
#include <functional>
#include <stdexcept>

class ThreadPool {
 public:
    ThreadPool(size_t);
    
    template<class F, class... Args>
    auto enqueueTasks(F&& f, Args&&... args) -> 
        std::future<typename std::result_of<F(Args...)>::type>;

    ~ThreadPool();

 private:
    std::vector<std::thread> workers; // Vector of workers threads
    std::queue<std::function<void()>> tasks; // Queue of tasks

    // Synchronisation
    std::mutex mutex;
    std::condition_variable condition_variable;
    bool stop;
};

ThreadPool::ThreadPool(size_t numThreads) : stop(false) {
    for (int i = 0; i < numThreads; i++) {
        workers.emplace_back(
        [this] () {
            // Loop infinitely until breaks
            while (true) {
                std::function<void()> task;
                { 
                    // Critical Section  
                    std::unique_lock<std::mutex> lock(this->mutex);
                    // Put the thread into sleep state if:
                    // 1) The thread pool is not in the shutting down state AND
                    // 2) The task queue is empty
                    this->condition_variable.wait(lock, [this] {
                        return this->stop || !this->tasks.empty();
                    });

                    // Return the thread if
                    // 1) The thread pool is in the shutting down state AND
                    // 2) The task queue is empty
                    if (this->stop && this->tasks.empty()) return;

                    // Grab the reference of the task at the front of the queue
                    // Move the task out of the task queue
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }
                // Execute the task
                task();
            }
        });
    }
}

/*
       +----------------------------------------------+
       |              std::packaged_task              |
       |    +-----------------------------------+     |
       |    |               INTERNAL STATE      |     |
       |    |   [ Result Value ] <---+          |     |
       |    |   [ Exception    ]     |          |     |
       |    |   [ Status Flag  ]     |          |     |
       |    +----------|-------------|----------+     |
       |               v             |                |
       |    +------------------------|----------+     |
       |    |          THE TASK                 |     |
       |    |   "return f(args...);"            |     |
       |    +-----------------------------------+     |
       +----------|-----------------------------------+
                  |
                  | (Connected via the Internal State)
                  v
       +-----------------------+
       |      std::future      |
       +-----------------------+
*/

template<class F, class... Args>
auto ThreadPool::enqueueTasks(F&& f, Args&&... args) -> 
    std::future<typename std::result_of<F(Args...)>::type> {
    using TaskResult = typename std::result_of<F(Args...)>::type;

    // Assemble task as packaged_task
    auto task = std::make_shared<std::packaged_task<TaskResult()> >(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    // Get the future value of the packaged_task
    std::future<TaskResult> futureTaskResult = task->get_future();

    {
        // Critical Section  
        std::unique_lock<std::mutex> lock(this->mutex);

        // If the thread pool is being shutting down, don't enqueue task into tasks queue
        if (stop) {
            throw std::runtime_error("Thread Pool is being shutting down. Cannot enqueue tasks!");
        }

        this->tasks.emplace([task]{(*task)();});

        // Wake a sleeping thread up
        this->condition_variable.notify_one();
    }

    return futureTaskResult;
}

ThreadPool::~ThreadPool() {
    {
        // Critical Section
        std::unique_lock<std::mutex> lock(this->mutex);
        this->stop = true;
    }

    // Wake up all threads
    this->condition_variable.notify_all();

    // Join in all worker threads
    for (std::thread& thread : this->workers) {
        thread.join();
    }
}

#endif