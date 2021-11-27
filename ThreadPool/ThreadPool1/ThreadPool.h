#ifndef __THREAD_POOL_H__
#define __THREAD_POOL_H__

/*
* 固定线程数量的线程池(C++11)
* 线程数量在构造函数中固定死，并不能自动收缩
*/

#include <vector>
#include <mutex>
#include <functional>
#include <thread>
#include <queue>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <future>

struct ThreadPool
{
    ThreadPool(int threads);
    ~ThreadPool();

    //任务放入线程池任务队列
    template<typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type>;

private:
    std::vector<std::thread> m_workers;        //用vector作为承载线程对象的容器
    std::queue<std::function<void()> > m_taskQueue; //任务队列，用std::function作为包装任务的容器
    std::mutex m_taskQueueMutex;               //任务队列使用的互斥锁
    std::condition_variable m_taskQueueCond;   //任务队列通知工作线程的信号量
    std::atomic<bool> m_stop;
};

ThreadPool::ThreadPool(int threads):m_stop(false)
{
    for(int i = 0; i < threads; ++i)
    {
        m_workers.emplace_back(//捕获this指针
            [this]  
            {
                for(;;)
                {
                    std::function<void()> task; //使用function作为任务容器

                    {
                        std::unique_lock<std::mutex> lock(m_taskQueueMutex);  //使用条件变量之前先上锁
                        m_taskQueueCond.wait(
                            lock, 
                            [this] 
                            { return m_stop || !m_taskQueue.empty(); }
                        );  //线程池停止运行时需要返回进行处理，任务队列不空时需要返回进行处理

                        if(m_stop && m_taskQueue.empty())  //只有当线程池停止运行且所有任务已经被处理掉了，工作线程才会结束执行
                            return;
                        
                        task = std::move(m_taskQueue.front()); //采用移动的方式.function支持移动也支持拷贝
                        m_taskQueue.pop();
                    }

                    task();  //执行任务
                }
            }
        );
    }
}

template<typename F, typename... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args)
    -> std::future<typename std::result_of<F(Args...)>::type>
{
    using resultType = typename std::result_of<F(Args...)>::type;  //任务执行的返回值类型

    auto task = std::make_shared<std::packaged_task<resultType()>>(
        std::bind(std::forward<F>(f), std::forward<Args>(args)...)  //将每个参数进行完美转发
    );

    std::future<resultType> res = task->get_future();

    {
        std::unique_lock<std::mutex> lock(m_taskQueueMutex);

        if(m_stop)
        {
            throw std::runtime_error("enqueue on stopped ThreadPool!");
        }

        //放入线程池工作队列的是匿名lambda，因为此时task是个智能指针，这个匿名lambda里值捕获了task智能指针
        m_taskQueue.emplace(
            [task]
            {
                (*task)(); //取智能指针指向的packaged_task对象，并执行
            }
        );
    }

    //通知在等待的工作线程
    m_taskQueueCond.notify_one();

    //返回结果期望
    return res;
}

ThreadPool::~ThreadPool()
{
    { //上锁，线程池可能还在工作
        std::unique_lock<std::mutex> lock(m_taskQueueMutex);
        m_stop = true;
    }

    m_taskQueueCond.notify_all();

    //在std::thread对象销毁之前，必须指明使用join还是detach进行回收，否则std::thread的析构函数会调用std::terminate()函数终止程序；
    for(auto& worker : m_workers)
    {
        worker.join();
    }
}

#endif