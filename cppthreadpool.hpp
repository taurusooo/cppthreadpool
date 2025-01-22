#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <future>
#include <vector>
#include <atomic>
#include <iostream>

namespace cppthreadpool
{
    template <typename TaskResturnType>
    struct SubmitResult
    {
        std::shared_future<TaskResturnType> future;
        bool success;
    };

    // ThreadPool：一个支持返回值的线程池类
    class ThreadPool
    {
    public:
        // 构造函数：初始化线程池，并创建指定数量的线程
        explicit ThreadPool(size_t minThreadCount = 2, 
                            size_t maxThreadCount = 0, 
                            size_t maxLoadDuration = 1000,
                            size_t maxIdleMsDuration = 1000*60*30,
                            size_t maxFullLoadCount = 5);

        // 析构函数：确保所有任务完成，并等待线程退出
        ~ThreadPool();

       // 提交任务到线程池，返回一个 std::future，用于获取任务结果，不带回调
        template <typename Func, typename... Args>
        SubmitResult<decltype(std::declval<Func>()(std::declval<Args>()...))> submit(Func&& func, Args&&... args);

        // 提交任务到线程池，返回一个 std::future，用于获取任务结果，带回调
        template <typename Func, typename Callback, typename... Args>
        SubmitResult<decltype(std::declval<Func>()(std::declval<Args>()...))> submit(Func&& func, Callback&& callback, Args&&... args);

        // 停止接受新任务，并等待所有已提交任务完成
        void shutdown();
        // 等待所有任务完成
        void wait();

    private:
        // 每个工作线程执行的函数
        void workerThread();
        // 负载监控线程
        void monitorLoad();

        std::vector<std::thread> m_threads;              // 工作线程集合
        std::queue<std::function<void()>> m_taskQueue;   // 任务队列

        std::mutex m_queueMutex;                         // 保护任务队列的互斥锁
        std::condition_variable m_condition;             // 条件变量，用于通知线程处理任务

        std::atomic<bool> m_isRunning;                   // 标志线程池运行状态


        size_t m_minThreadCount;
        size_t m_maxThreadCount;
        std::chrono::milliseconds m_maxLoadMsDuration;   // 最大负载持续时间
        std::chrono::milliseconds m_maxIdleMsDuration;     // 最大空闲线程持续时间
        std::chrono::milliseconds m_currentIdleDuration; // 当前出现空闲线程持续时间
        size_t m_maxFullLoadCount;                       // 最负载达到最大值次数
        std::atomic<size_t> m_currentFullLoadCount;      // 当前负载达到最大值次数
        std::atomic<size_t> m_currentFreeThreadCount;    // 当前空闲线程数量
        std::atomic<bool> m_isMonitoring;                // 标志是否正在监控负载
        std::thread m_monitorThread;                     // 负载监控线程
    };

    // 构造函数实现
    ThreadPool::ThreadPool(size_t minThreadCount, 
                           size_t maxThreadCount, 
                           size_t maxLoadMsDuration, 
                           size_t maxIdleMsDuration,
                           size_t maxFullLoadCount)
        : m_isRunning(true), m_minThreadCount(minThreadCount), 
          m_maxThreadCount(maxThreadCount==0 ? minThreadCount : maxThreadCount),
          m_maxLoadMsDuration(std::chrono::milliseconds(maxLoadMsDuration)), 
          m_maxIdleMsDuration(std::chrono::milliseconds(maxIdleMsDuration)),
          m_maxFullLoadCount(maxFullLoadCount),
        
          m_currentFullLoadCount(0), 
          m_currentIdleDuration(std::chrono::milliseconds(0)),
          m_isMonitoring(true)
    {
        if (m_minThreadCount == 0 || m_maxThreadCount == 0)
            throw std::invalid_argument("Thread pool must have at least one thread.");

        if (m_minThreadCount > m_maxThreadCount)
            throw std::invalid_argument("Minimum thread count cannot be greater than maximum thread count.");

        if (maxLoadMsDuration == 0 || maxIdleMsDuration == 0)
            throw std::invalid_argument("Maximum load duration and idle duration must be non-zero.");

        if (maxFullLoadCount == 0)
            throw std::invalid_argument("Maximum full load count must be greater than zero.");


        try
        {
            // 创建初始线程
            for (size_t i = 0; i < minThreadCount; ++i)
            {
                m_threads.emplace_back(&ThreadPool::workerThread, this);
            }

            m_currentFreeThreadCount = m_threads.size();

            // 启动负载监控线程
            m_monitorThread = std::thread(&ThreadPool::monitorLoad, this);
        }
        catch (const std::exception& e)
        {
            std::cerr << "Error initializing ThreadPool: " << e.what() << std::endl;
            // 往上重新抛出该异常(e)
            throw; 
        }

    }

    ThreadPool::~ThreadPool()
    {
        // 停止线程池,销毁所有线程
        shutdown();
    }

    void ThreadPool::wait()
    {
        // 等待所有工作线程完成
        while (m_currentFreeThreadCount != m_threads.size())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    // 停止接受新任务，并等待所有已提交任务完成
    void ThreadPool::shutdown()
    {
        {
            
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (!m_isRunning)
                return;
                
            m_isRunning = false;
        }

        m_isMonitoring = false;
        if(m_monitorThread.joinable())
            m_monitorThread.join();

        m_condition.notify_all();
        for (auto& thread : m_threads)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
        m_threads.clear();
        // std::cout << "cppthreadpoool shutdown sucess" << std::endl;
    }

    // 通用模板：处理非 void 类型
    template <typename T, typename Callback>
    typename std::enable_if<!std::is_void<T>::value>::type
    handleCallback(const std::shared_future<T>& result, Callback&& callback) 
    {
        try 
        {
            // 获取任务结果并传递给回调函数
            callback(result.get());
        } 
        catch (const std::exception& e) 
        {
            std::cerr << "Callback exception: " << e.what() << std::endl;
        } 
        catch (...) 
        {
            std::cerr << "Callback encountered an unknown exception." << std::endl;
        }
    }

    // 特化模板：处理 void 类型
    template <typename T, typename Callback>
    typename std::enable_if<std::is_void<T>::value>::type
    handleCallback(const std::shared_future<T>& result, Callback&& callback) 
    {
        try 
        {
            // 确保任务执行完成
            result.get();

            // 调用回调函数
            callback();
        } 
        catch (const std::exception& e) 
        {
            std::cerr << "Callback exception: " << e.what() << std::endl;
        } 
        catch (...) 
        {
            std::cerr << "Callback encountered an unknown exception." << std::endl;
        }
    }

    // 修改 submit 函数带回调
    template <typename Func, typename Callback, typename... Args>
    SubmitResult<decltype(std::declval<Func>()(std::declval<Args>()...))> ThreadPool::submit(
        Func&& func, Callback&& callback, Args&&... args)
    {
        using ReturnType = decltype(std::declval<Func>()(std::declval<Args>()...));

        try
        {
            // 创建一个 packaged_task 来封装任务，并返回一个 future
            auto task = std::make_shared<std::packaged_task<ReturnType()>>(
                std::bind(std::forward<Func>(func), std::forward<Args>(args)...));

            // 获取与任务相关的 future
            std::shared_future<ReturnType> result = task->get_future();

            // 创建一个任务，执行完毕后调用回调函数
            auto callbackTask = [task, callback, result]() 
            {
                try 
                {
                    (*task)();
                } 
                catch (const std::exception& e) 
                {
                    std::cerr << "Task exception: " << e.what() << std::endl;
                } 
                catch (...) 
                {
                    std::cerr << "Task encountered an unknown exception." << std::endl;
                }

                handleCallback<ReturnType>(result, callback);
            };

            {
                // 加锁以保护任务队列
                std::lock_guard<std::mutex> lock(m_queueMutex);
                if (!m_isRunning) 
                {
                    return {std::shared_future<ReturnType>(), false};
                }

                m_taskQueue.emplace(callbackTask);  // 添加任务到队列
            }

            m_condition.notify_one();  // 通知工作线程

            return {std::move(result), true};  //显式移动,优化shared_future引用计数维护开销
        }
        catch (...)
        {
            return {std::shared_future<ReturnType>(), false};
        }
    }

   // 修改 submit 函数不带回调
    template <typename Func, typename... Args>
    SubmitResult<decltype(std::declval<Func>()(std::declval<Args>()...))> ThreadPool::submit(
        Func&& func, Args&&... args)
    {
        using ReturnType = decltype(std::declval<Func>()(std::declval<Args>()...));
        try
        {
            // 创建一个 packaged_task 来封装任务，并返回一个 future
            auto task = std::make_shared<std::packaged_task<ReturnType()>>(
                std::bind(std::forward<Func>(func), std::forward<Args>(args)...));

            // 获取与任务相关的 future
            std::shared_future<ReturnType> result = task->get_future();

            {
                // 加锁以保护任务队列
                std::lock_guard<std::mutex> lock(m_queueMutex);
                if (!m_isRunning)
                {
                    return {std::shared_future<ReturnType>(), false};
                }

                // 将任务封装为一个无参的可调用对象，加入队列
                m_taskQueue.emplace([task]() { (*task)(); });
            }

            // 通知一个工作线程处理新任务
            m_condition.notify_one();

            return {std::move(result), true};  // 返回 future 和成功标志
        }
        catch (...)
        {
            return {std::shared_future<ReturnType>(), false};
        }
    }

    // 工作线程执行的函数
    void ThreadPool::workerThread()
    {
        while (true)
        {
            // std::this_thread::sleep_for(std::chrono::milliseconds(100));

            std::function<void()> task;

            {
                // 锁定队列互斥锁，等待任务或停止信号
                std::unique_lock<std::mutex> lock(m_queueMutex);

                m_condition.wait(lock, [this]() { return !m_isRunning || !m_taskQueue.empty(); });

                // 如果线程池停止运行且任务队列为空，退出线程
                if (!m_isRunning && m_taskQueue.empty())
                {
                    return;
                }

                // 从任务队列中取出一个任务
                task = std::move(m_taskQueue.front());
                m_taskQueue.pop();
            }

            --m_currentFreeThreadCount;
            // std::cerr << "start: " << m_currentFreeThreadCount << std::endl;
            try 
            {
                task();
            } 
            catch (const std::exception& e) 
            {
                std::cerr << "Task exception: " << e.what() << std::endl;
            } 
            catch (...) 
            {
                std::cerr << "Task encountered an unknown exception." << std::endl;
            }
            // std::cerr << "end: " << m_currentFreeThreadCount << std::endl;

            ++m_currentFreeThreadCount;
        }
    }

   void ThreadPool::monitorLoad()
    {
        while (m_isMonitoring)
        {
            std::this_thread::sleep_for(m_maxLoadMsDuration);
            // std::cerr << "current thread count: " << m_threads.size()<<"  ";
            // std::cerr <<"idle threads: " << m_currentFreeThreadCount << std::endl;

            // 如果有没有空闲线程的情况连续多次出现达到阈值且没有达到最大线程数量，增加一个线程
            if (m_currentFreeThreadCount == 0 && m_threads.size() < m_maxThreadCount)
            {
                ++m_currentFullLoadCount;
                m_currentIdleDuration = std::chrono::milliseconds(0);   // 重新计时
                if(m_currentFullLoadCount==m_maxFullLoadCount)
                {
                    // 如果没有空闲线程并且线程池没有达到最大线程数，增加一个线程
                    m_threads.emplace_back(&ThreadPool::workerThread, this);
                    // std::cerr << "Adding a new thread. Current thread count: " << m_threads.size() << std::endl;
                    // 重新计数
                    m_currentFullLoadCount = 0;
                    ++m_currentFreeThreadCount;
                }
                continue;
            }

            if(m_currentFullLoadCount>0) --m_currentFullLoadCount;
            else m_currentFullLoadCount = 0;

            // 如果有空闲线程大于2/3线程池数
            if (m_currentFreeThreadCount > m_threads.size()*2/3)
            {
                // 累积空闲时间
                m_currentIdleDuration+=m_maxLoadMsDuration;
                // 达到累积时间
                if(m_currentIdleDuration>=m_maxIdleMsDuration)
                {
                    // ...TODO: 减少一个线程
                }
            }
            // 健康负荷
            else
            {
                m_currentIdleDuration  = std::chrono::milliseconds(0);
            }
        }
        // std::cerr << "monitor thread end " << std::endl;
    }
}