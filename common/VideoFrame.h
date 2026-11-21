#pragma once

#include "VideoFormat.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace uvtf
{

constexpr std::size_t MAX_VIDEO_PLANES = 4;

struct VideoPlane
{
    std::size_t offset = 0;
    int stride = 0;
    int width = 0;
    int height = 0;
};

struct VideoFrame
{
    VideoFormat format;
    std::vector<std::uint8_t> data;
    std::array<VideoPlane, MAX_VIDEO_PLANES> planes{};
    std::size_t planeCount = 0;
    std::int64_t presentationTimestamp = 0;
    std::int64_t duration = 0;

    bool allocate(const VideoFormat& requestedFormat)
    {
        if (!requestedFormat.isValid())
            return false;

        const std::size_t width =
            static_cast<std::size_t>(requestedFormat.width);
        const std::size_t height =
            static_cast<std::size_t>(requestedFormat.height);
        const std::size_t maxSize =
            std::numeric_limits<std::size_t>::max();

        std::vector<std::uint8_t> newData;
        std::array<VideoPlane, MAX_VIDEO_PLANES> newPlanes{};
        std::size_t newPlaneCount = 0;

        try
        {
            if (requestedFormat.pixelFormat == PixelFormat::YUV420P)
            {
                if ((requestedFormat.width & 1) || (requestedFormat.height & 1))
                    return false;

                if (height > maxSize / width)
                    return false;

                const std::size_t ySize = width * height;
                const std::size_t chromaWidth = width / 2;
                const std::size_t chromaHeight = height / 2;

                if (chromaHeight > maxSize / chromaWidth)
                    return false;

                const std::size_t chromaSize = chromaWidth * chromaHeight;
                if (chromaSize > (maxSize - ySize) / 2)
                    return false;

                newData.assign(ySize + 2 * chromaSize, 0);
                newPlaneCount = 3;
                newPlanes[0] = {0, requestedFormat.width,
                    requestedFormat.width, requestedFormat.height};
                newPlanes[1] = {ySize, requestedFormat.width / 2,
                    requestedFormat.width / 2, requestedFormat.height / 2};
                newPlanes[2] = {ySize + chromaSize, requestedFormat.width / 2,
                    requestedFormat.width / 2, requestedFormat.height / 2};
            }
            else if (requestedFormat.pixelFormat == PixelFormat::BGRA32)
            {
                if (width > maxSize / 4 ||
                    width > static_cast<std::size_t>(std::numeric_limits<int>::max() / 4) ||
                    height > maxSize / (width * 4))
                    return false;

                const std::size_t stride = width * 4;
                newData.assign(stride * height, 0);
                newPlaneCount = 1;
                newPlanes[0] = {
                    0,
                    static_cast<int>(stride),
                    requestedFormat.width,
                    requestedFormat.height
                };
            }
            else
            {
                return false;
            }
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
        catch (const std::length_error&)
        {
            return false;
        }

        data.swap(newData);
        format = requestedFormat;
        planes = newPlanes;
        planeCount = newPlaneCount;
        presentationTimestamp = 0;
        duration = 0;
        return true;
    }

    bool isValid() const
    {
        if (!format.isValid() || data.empty() ||
            planeCount == 0 || planeCount > MAX_VIDEO_PLANES)
            return false;

        for (std::size_t i = 0; i < planeCount; ++i)
        {
            const auto& plane = planes[i];
            if (plane.stride <= 0 || plane.width <= 0 ||
                plane.height <= 0 || plane.width > plane.stride)
                return false;

            const std::size_t stride =
                static_cast<std::size_t>(plane.stride);
            const std::size_t height =
                static_cast<std::size_t>(plane.height);

            if (plane.offset > data.size())
                return false;

            const std::size_t remaining = data.size() - plane.offset;
            if (height > remaining / stride)
                return false;
        }

        return true;
    }
};

}
