
#pragma once

#include "VideoFrame.h"
#include "VideoIOStatus.h"

namespace uvtf
{

class IVideoInput
{
public:

    virtual ~IVideoInput() = default;

    // Initialize the input source.
    virtual bool open() = 0;

    // Retrieve the next available video frame.
    virtual VideoIOStatus read(VideoFrame& frame) = 0;

    // Release input resources.
    virtual void close() noexcept = 0;

    // Check input state.
    virtual bool isOpen() const noexcept = 0;

};

}
