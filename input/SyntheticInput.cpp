
#include "SyntheticInput.h"

#include <cstddef>
#include <cstdint>

namespace uvtf
{

SyntheticInput::SyntheticInput(
    const VideoFormat& format,
    std::uint64_t maxFrames
)
    : format_(format),
      maxFrames_(maxFrames)
{
}

bool SyntheticInput::open()
{
    if (!format_.isValid() ||
        format_.pixelFormat != PixelFormat::YUV420P ||
        format_.width % 2 != 0 ||
        format_.height % 2 != 0 ||
        maxFrames_ == 0)
    {
        return false;
    }

    frameIndex_ = 0;
    opened_ = true;

    return true;
}

VideoIOStatus SyntheticInput::read(VideoFrame& frame)
{
    if (!opened_)
    {
        return VideoIOStatus::Error;
    }

    if (frameIndex_ >= maxFrames_)
    {
        return VideoIOStatus::EndOfStream;
    }

    // Allocate only when the destination frame
    // does not already contain a compatible buffer.
    if (!frame.isValid() ||
        frame.format.width != format_.width ||
        frame.format.height != format_.height ||
        frame.format.pixelFormat != format_.pixelFormat ||
        frame.format.frameRateNumerator != format_.frameRateNumerator ||
        frame.format.frameRateDenominator != format_.frameRateDenominator)
    {
        if (!frame.allocate(format_))
        {
            return VideoIOStatus::Error;
        }
    }

    generateFrame(frame);

    // PTS uses frame ticks: one tick per frame.
    frame.presentationTimestamp =
        static_cast<std::int64_t>(frameIndex_);

    frame.duration = 1;

    ++frameIndex_;

    return VideoIOStatus::Ok;
}

void SyntheticInput::generateFrame(VideoFrame& frame)
{
    auto& yPlane = frame.planes[0];
    auto& uPlane = frame.planes[1];
    auto& vPlane = frame.planes[2];

    // Generate Y plane with a moving bright vertical bar.
    const int barWidth = format_.width / 8;

    const int barPosition =
        static_cast<int>(
            (frameIndex_ * 8) %
            static_cast<std::uint64_t>(format_.width)
        );

    for (int y = 0; y < yPlane.height; ++y)
    {
        for (int x = 0; x < yPlane.width; ++x)
        {
            const int gradient =
                32 + (x * 160 / format_.width);

            const bool insideBar =
                x >= barPosition &&
                x < barPosition + barWidth;

            const std::uint8_t value =
                static_cast<std::uint8_t>(
                    insideBar ? 220 : gradient
                );

            const std::size_t offset =
                yPlane.offset +
                static_cast<std::size_t>(y) * yPlane.stride +
                static_cast<std::size_t>(x);

            frame.data[offset] = value;
        }
    }

    // Generate U and V chroma planes.
    for (int y = 0; y < uPlane.height; ++y)
    {
        for (int x = 0; x < uPlane.width; ++x)
        {
            const std::size_t uOffset =
                uPlane.offset +
                static_cast<std::size_t>(y) * uPlane.stride +
                static_cast<std::size_t>(x);

            const std::size_t vOffset =
                vPlane.offset +
                static_cast<std::size_t>(y) * vPlane.stride +
                static_cast<std::size_t>(x);

            frame.data[uOffset] = 128;
            frame.data[vOffset] = 128;
        }
    }
}

void SyntheticInput::close() noexcept
{
    opened_ = false;
    frameIndex_ = 0;
}

bool SyntheticInput::isOpen() const noexcept
{
    return opened_;
}

}
