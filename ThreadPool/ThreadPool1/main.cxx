#include <iostream>
#include <vector>
#include <chrono>
#include "ThreadPool.h"

int main()
{
    ThreadPool thp(5);

    std::vector<std::future<int> > results;
    for(int i = 0; i <101; ++i)
    {
        results.push_back(thp.enqueue(
                [i]
                {
                    std::cout<<"hello i:"<<i<<std::endl;
                    //std::this_thread::sleep_for(std::chrono::seconds(1));
                    //std::cout<<"world i:"<<i<<std::endl;
                    return i*i;
                }
            )
        );
    }

    std::this_thread::sleep_for(std::chrono::seconds(10));

    for(auto & r : results)
    {
        std::cout<<r.get()<<' ';
    }

    std::cout<<std::endl;

    return 0;
}