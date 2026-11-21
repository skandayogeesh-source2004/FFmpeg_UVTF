
#pragma once

#include "VideoFrame.h"
#include "VideoIOStatus.h"

namespace uvtf
{

class IVideoOutput
{
public:

    virtual ~IVideoOutput() = default;

    // Initialize the output destination.
    virtual bool open() = 0;

    // Submit a video frame.
    virtual VideoIOStatus write(const VideoFrame& frame) = 0;

    // Release output resources.
    virtual void close() noexcept = 0;

    // Check output state.
    virtual bool isOpen() const noexcept = 0;

};

}
