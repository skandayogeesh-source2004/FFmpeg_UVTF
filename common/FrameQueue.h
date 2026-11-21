
#pragma once

#include "VideoFrame.h"
#include <stdexcept>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace uvtf
{

enum class QueuePushResult
{
    Accepted,
    Full,
    Closed
};

class FrameQueue
{
public:

    explicit FrameQueue(std::size_t capacity)
        : capacity_(capacity)
    {
	if (capacity == 0)
	{
	    throw std::invalid_argument(
                 "FrameQueue capacity must be greater than zero"
            );
	}
    }

    // Non-blocking insertion.
    QueuePushResult tryPush(VideoFrame&& frame)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            if (closed_)
            {
                return QueuePushResult::Closed;
            }

            if (queue_.size() >= capacity_)
            {
                return QueuePushResult::Full;
            }

            queue_.push_back(std::move(frame));
        }

        notEmpty_.notify_one();

        return QueuePushResult::Accepted;
    }

    // Blocking insertion when the queue is full.
    bool waitPush(VideoFrame&& frame)
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);

            notFull_.wait(lock, [this]()
            {
                return closed_ ||
                       queue_.size() < capacity_;
            });

            if (closed_)
            {
                return false;
            }

            queue_.push_back(std::move(frame));
        }

        notEmpty_.notify_one();

        return true;
    }

    // Blocking frame retrieval.
    bool waitPop(VideoFrame& frame)
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);

            notEmpty_.wait(lock, [this]()
            {
                return closed_ || !queue_.empty();
            });

            if (queue_.empty())
            {
                return false;
            }

            frame = std::move(queue_.front());

            queue_.pop_front();
        }

        notFull_.notify_one();

        return true;
    }

    // Graceful shutdown.
    void close()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);

            closed_ = true;
        }

        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        return queue_.size();
    }

    std::size_t capacity() const noexcept
    {
        return capacity_;
    }

    bool isClosed() const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        return closed_;
    }

private:

    const std::size_t capacity_;

    mutable std::mutex mutex_;

    std::condition_variable notEmpty_;
    std::condition_variable notFull_;

    std::deque<VideoFrame> queue_;

    bool closed_ = false;
};

}
