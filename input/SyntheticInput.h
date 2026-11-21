
#pragma once

#include "../common/IVideoInput.h"

#include <cstdint>

namespace uvtf
{

class SyntheticInput : public IVideoInput
{
public:

    explicit SyntheticInput(
        const VideoFormat& format,
        std::uint64_t maxFrames = 90
    );

    bool open() override;

    VideoIOStatus read(VideoFrame& frame) override;

    void close() noexcept override;

    bool isOpen() const noexcept override;

private:

    void generateFrame(VideoFrame& frame);

    VideoFormat format_;

    std::uint64_t maxFrames_;
    std::uint64_t frameIndex_ = 0;

    bool opened_ = false;
};

}
