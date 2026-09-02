#pragma once

#include <cstdint>
#include <queue>
#include <mutex>
#include <cstddef>
#include <condition_variable>


struct Frame{
    unsigned char *data;
    size_t size;

    Frame():data(nullptr),size(0){}

    Frame(unsigned char *d,size_t z):data(d),size(z){}
};


class FrameQueue {
    public:
    explicit FrameQueue(size_t maxSize) : maxSize_(maxSize) {}

    void push(Frame f)
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if(q_.size()>=maxSize_)
            {
                delete[] q_.front().data;
                q_.pop();
            }
            q_.push(f);
        }
        cond_.notify_all();
    }

    bool pop(Frame &out)
    {
        std::unique_lock<std::mutex> lk(mtx_);
        cond_.wait(lk,[this]{return !q_.empty() || done_;});

        if(q_.empty())
        {
            return false;
        }
        out=q_.front();
        q_.pop();
        return true;
    }

    void close()
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            done_=true;
        }
        cond_.notify_all();
    }



    private:
    size_t maxSize_;
    std::queue<Frame> q_;
    std::mutex mtx_;
    bool done_=false;
    std::condition_variable cond_;



};