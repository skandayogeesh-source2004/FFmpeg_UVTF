
#pragma once

#include "IVideoInput.h"

#include <memory>
#include <string>

namespace uvtf
{

class FileInput final : public IVideoInput
{
public:
    explicit FileInput(const std::string& filePath);

    ~FileInput() override;

    FileInput(const FileInput&) = delete;
    FileInput& operator=(const FileInput&) = delete;

    bool open() override;

    VideoIOStatus read(VideoFrame& frame) override;

    void close() noexcept override;

    bool isOpen() const noexcept override;

    const std::string& getLastError() const noexcept;

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;

    std::string filePath_;
    std::string lastError_;
};

}
