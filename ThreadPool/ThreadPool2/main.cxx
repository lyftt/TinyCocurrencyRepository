#include "ThreadPool.h"
#include <iostream>

int main()
{
    std::vector<std::shared_ptr<std::future<int>>> results;
    {
        auto p = ThreadPool::Create(5, 10, std::chrono::seconds(1), 10000);
        p->start();

        for(int i = 0; i <101; ++i)
        {
            results.push_back(p->enqueue(
                    [i]
                    {
                        std::cout<<"hello i:"<<i<<std::endl;
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        return i*i;
                    }
                )
            );
        }

        p->stop();
        std::cout<<"coutn:"<<p.use_count()<<std::endl;
    }

    std::this_thread::sleep_for(std::chrono::seconds(5));

    for(auto & r : results)
    {
        std::cout<<(*r).get()<<' ';
    }

    std::cout<<std::endl;

    return 0;
}