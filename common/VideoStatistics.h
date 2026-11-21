#pragma once

#include <chrono>
#include <cstdint>

namespace uvtf
{

// Pipeline-level counters. Packet counts are explicitly scoped to the FFmpeg
// encoder/muxer and demuxer APIs; they are not raw UDP datagram counts.
class VideoStatistics
{
public:
    using Clock = std::chrono::steady_clock;

    void start() noexcept
    {
        startedAt_ = Clock::now();
        stoppedAt_ = startedAt_;
        running_ = true;
    }

    void stop() noexcept
    {
        if (running_)
        {
            stoppedAt_ = Clock::now();
            running_ = false;
        }
    }

    double elapsedSeconds() const noexcept
    {
        const auto end = running_ ? Clock::now() : stoppedAt_;
        return std::chrono::duration<double>(end - startedAt_).count();
    }

    double framesPerSecond(std::uint64_t frames) const noexcept
    {
        const double seconds = elapsedSeconds();
        return seconds > 0.0 ? static_cast<double>(frames) / seconds : 0.0;
    }

    std::uint64_t inputFrames = 0;
    std::uint64_t submittedFrames = 0;
    std::uint64_t encodedPackets = 0;
    std::uint64_t demuxedVideoPackets = 0;
    std::uint64_t decodedFrames = 0;
    std::uint64_t renderedFrames = 0;
    std::uint64_t decoderErrors = 0;
    std::uint64_t renderErrors = 0;

private:
    Clock::time_point startedAt_{};
    Clock::time_point stoppedAt_{};
    bool running_ = false;
};

}
