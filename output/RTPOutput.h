
#pragma once

#include "IVideoOutput.h"

#include <cstdint>
#include <memory>
#include <string>

namespace uvtf
{

class RTPOutput final : public IVideoOutput
{
public:
    RTPOutput(
        const VideoFormat& format,
        const std::string& destinationUrl,
        const std::string& sdpFilePath = "uvtf_sender.sdp",
        int bitRate = 4000000
    );

    ~RTPOutput() override;

    RTPOutput(const RTPOutput&) = delete;
    RTPOutput& operator=(const RTPOutput&) = delete;

    bool open() override;

    VideoIOStatus write(const VideoFrame& frame) override;

    void close() noexcept override;

    bool isOpen() const noexcept override;

    const std::string& getLastError() const noexcept;
    std::uint64_t getSubmittedFrameCount() const noexcept;
    std::uint64_t getEncodedPacketCount() const noexcept;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;

    VideoFormat format_;
    std::string destinationUrl_;
    std::string sdpFilePath_;
    std::string lastError_;

    int bitRate_;
};

}
