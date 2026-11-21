#pragma once

#include "IVideoInput.h"

#include <cstdint>
#include <memory>
#include <string>

namespace uvtf
{

class RTPInput final : public IVideoInput
{
public:
    explicit RTPInput(const std::string& sdpFilePath);
    ~RTPInput() override;

    RTPInput(const RTPInput&) = delete;
    RTPInput& operator=(const RTPInput&) = delete;

    bool open() override;
    VideoIOStatus read(VideoFrame& frame) override;
    void close() noexcept override;
    bool isOpen() const noexcept override;

    // Call when the user requests shutdown. The next read() drains the
    // decoder instead of accepting additional RTP packets.
    void requestStop() noexcept;
    bool isDraining() const noexcept;

    // Automatic end-of-stream detection after receiving at least one
    // video packet. Set to 0 to disable idle-based shutdown.
    void setIdleTimeoutMs(std::uint32_t timeoutMs) noexcept;

    const std::string& getLastError() const noexcept;
    std::uint64_t getDemuxedVideoPacketCount() const noexcept;
    std::uint64_t getDecodedFrameCount() const noexcept;
    std::uint64_t getDecoderErrorCount() const noexcept;

public:
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    std::string sdpFilePath_;
    std::string lastError_;
};

}
