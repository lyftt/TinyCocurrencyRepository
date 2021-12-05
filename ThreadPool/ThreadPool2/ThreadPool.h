#ifndef __THREAD_POOL_H__
#define __THREAD_POOL_H__

#include <chrono>
#include <thread>
#include <list>
#include <queue>
#include <functional>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <iostream>
#include <future>

struct ThreadPool : public std::enable_shared_from_this<ThreadPool>
{
    //线程池配置参数
    struct ThreadPoolConfig
    {
        int m_coreThreads;
        int m_maxThreads;
        std::chrono::seconds m_recycleTimeout;
        int m_maxtasks;
    };

    //单个线程标志(核心线程还是扩展出来的线程)
    enum class ThreadFlag
    {
        kCoreThread = 0,
        kextendThread
    };

    //单个线程状态标识
    enum class ThreadState
    {
        kInit = 0,
        kWaiting,
        kRunning,
        kStop
    };

    using ThreadPtr = std::shared_ptr<std::thread>;       //线程对象指针类型
    using AtomicThreadState = std::atomic<ThreadState>;   //线程状态类型
    using AtomicThreadFlag = std::atomic<ThreadFlag>;     //线程标志类型

    //单个线程的信息
    struct ThreadInfo
    {
        ThreadPtr m_threadPtr;            //线程指针
        AtomicThreadState m_threadState;  //线程状态
        AtomicThreadFlag m_threadFlag;    //线程标志

        ThreadInfo()
        {
            m_threadPtr = nullptr;
            m_threadState.store(ThreadState::kInit);
        }
    };

    using ThreadInfoPtr = std::shared_ptr<ThreadInfo>;  //单个线程信息类型

    ~ThreadPool();

    //创建一个线程池对象
    static std::shared_ptr<ThreadPool> Create(int coreThreads, int maxThreads, std::chrono::seconds recycleTimeout, int maxTasks);

    //启动线程池
    bool start();

    //停止线程池
    void stop();

    //任务放入线程池任务队列
    template<typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args) -> std::shared_ptr<std::future<typename std::result_of<F(Args...)>::type>>;

private:
    ThreadPool(int coreThreads, int maxThreads, std::chrono::seconds recycleTimeout, int maxTasks = 1000000);

    //创建一个线程
    void addThread();
    void addThreadByFlag(ThreadFlag flag);

private:
    ThreadPoolConfig     m_config;      //线程池配置

    std::queue<std::function<void()>> m_tasks; //任务队列
    std::mutex  m_tasksMutex;                  //任务队列的保护锁
    std::condition_variable m_tasksCond;       //任务队列的条件变量

    std::atomic<int> m_waitThreadsNum;    //当前处于等待状态的线程数量
    std::atomic<int> m_totalThreadsNum;   //总的线程数量
    std::atomic<bool> m_stop;             //线程池停止标志
};

std::shared_ptr<ThreadPool> ThreadPool::Create(int coreThreads, int maxThreads, std::chrono::seconds recycleTimeout, int maxTasks)
{
    //return std::make_shared<ThreadPool>(coreThreads, maxThreads, recycleTimeout, maxTasks);   //因为构造函数是私有的，所以无法直接调用make_shared
    std::shared_ptr<ThreadPool> ptr(new ThreadPool(coreThreads, maxThreads, recycleTimeout, maxTasks));
    return ptr;
}

ThreadPool::ThreadPool(int coreThreads, int maxThreads, std::chrono::seconds recycleTimeout, int maxTasks)
{
    m_config.m_coreThreads = coreThreads;
    m_config.m_maxThreads = maxThreads;
    m_config.m_recycleTimeout = recycleTimeout;
    m_config.m_maxtasks = maxTasks;

    m_waitThreadsNum.store(0);
    m_totalThreadsNum.store(0);
    m_stop.store(false);
}

ThreadPool::~ThreadPool()
{
    std::cout<<"m_waitThreadsNum:"<<m_waitThreadsNum<<std::endl;
    std::cout<<"m_totalThreadsNum:"<<m_totalThreadsNum<<std::endl;
}

void ThreadPool::addThread()
{
    addThreadByFlag(ThreadFlag::kCoreThread);  //创建核心线程
}

void ThreadPool::addThreadByFlag(ThreadFlag flag)
{
    std::shared_ptr<ThreadInfo> threadInfoPtr = std::make_shared<ThreadInfo>();  //创建线程信息对象
    threadInfoPtr->m_threadFlag = flag;
    threadInfoPtr->m_threadState = ThreadState::kInit;

    //捕获线程池指针和线程信息对象指针
    auto p = shared_from_this();
    auto func = [p, threadInfoPtr]
    {
        for(;;)
        {
            if(threadInfoPtr->m_threadState.load() == ThreadState::kStop)
            {
                break;
            }

            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(p->m_tasksMutex);

                ++p->m_waitThreadsNum;   //等待线程数量加一

                if(threadInfoPtr->m_threadFlag.load() == ThreadFlag::kCoreThread)   //核心线程
                {
                    bool ret = p->m_tasksCond.wait_for(lock, std::chrono::milliseconds(100), [p, threadInfoPtr]
                        {
                            return (!p->m_tasks.empty()) || (threadInfoPtr->m_threadState.load() == ThreadState::kStop) || (p->m_stop);  //任务队列非空、线程处于停止状态、线程池停止工作 的时候返回
                        }
                    );

                    //等待线程数量减一
                    --p->m_waitThreadsNum;   
                    
                    //超时时间到，但是没有条件满足
                    if(!ret)
                    {
                        continue;
                    }

                    //线程的运行状态为stop
                    if(threadInfoPtr->m_threadState.load() == ThreadState::kStop)
                    {
                        break;
                    }

                    //线程池停止且任务队列已经为空
                    if(p->m_stop && p->m_tasks.empty())  
                    {
                        threadInfoPtr->m_threadState.store(ThreadState::kStop);   //设置线程运行状态
                        break;
                    }

                    if(!p->m_tasks.empty())
                    {
                        task = std::move(p->m_tasks.front());
                        p->m_tasks.pop();
                    }

                    threadInfoPtr->m_threadState.store(ThreadState::kRunning);   //设置线程运行状态
                }
                else   //非核心线程
                {
                    bool ret = p->m_tasksCond.wait_for(lock, p->m_config.m_recycleTimeout, [p, threadInfoPtr]
                        {
                            return (!p->m_tasks.empty()) || (threadInfoPtr->m_threadState.load() == ThreadState::kStop) || (p->m_stop);  //任务队列非空、线程处于停止状态、线程池停止工作 的时候返回
                        }
                    );

                    //等待线程数量减一
                    --p->m_waitThreadsNum; 

                    //超时时间到，但是没有条件满足
                    if(!ret)
                    {
                        threadInfoPtr->m_threadState.store(ThreadState::kStop);
                    }

                    //线程池停止
                    if(p->m_stop)
                    {
                        threadInfoPtr->m_threadState.store(ThreadState::kStop);
                    }

                    if(threadInfoPtr->m_threadState.load() == ThreadState::kStop)
                    {
                        break;
                    }

                    if(!p->m_tasks.empty())
                    {
                        task = std::move(p->m_tasks.front());
                        p->m_tasks.pop();
                    }

                    threadInfoPtr->m_threadState.store(ThreadState::kRunning);   //设置线程状态
                }
            }

            if(task)
            {
                task();  //执行任务
            }
        }

        --p->m_totalThreadsNum;  //线程数量减一
        std::cout<<"m_totalThreadsNum:"<<p->m_totalThreadsNum<<std::endl;
    };

    //创建线程
    threadInfoPtr->m_threadPtr = std::make_shared<std::thread>(std::move(func));
    threadInfoPtr->m_threadPtr->detach();
    ++m_totalThreadsNum;   //线程数量加一
    std::cout<<"create:"<<p->m_totalThreadsNum<<std::endl;
}

template<typename F, typename... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args) -> std::shared_ptr<std::future<typename std::result_of<F(Args...)>::type>>
{
    if(m_stop)
    {
        return nullptr;
    }

    //判断线程池是否繁忙，繁忙则创建额外线程
    if(m_waitThreadsNum.load() == 0 && m_totalThreadsNum.load() < m_config.m_maxThreads)
    {
        addThreadByFlag(ThreadFlag::kextendThread);
    }

    using resultType = typename std::result_of<F(Args...)>::type;

    auto task = std::make_shared<std::packaged_task<resultType()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)
    );

    std::future<resultType> res = task->get_future();

    {
        std::unique_lock<std::mutex> lock(m_tasksMutex);

        //放入任务对立
        m_tasks.emplace(
            [task]
            {
                (*task)();
            }
        );
    }

    //通知
    m_tasksCond.notify_one();

    //返回结果期望
    return std::make_shared<std::future<resultType>>(std::move(res));
}

bool ThreadPool::start()
{
    int coreThreads = m_config.m_coreThreads;

    while(coreThreads-- > 0)
    {
        addThread();
    }

    return true;
}

void ThreadPool::stop()
{
    {
        std::unique_lock<std::mutex> lock(m_tasksMutex);
        m_stop = true;
    }
}

#endif