
#include "FileInput.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

namespace uvtf
{

struct FileInput::Impl
{
    AVFormatContext* formatContext = nullptr;
    AVCodecContext* codecContext = nullptr;
    const AVCodec* decoder = nullptr;

    AVPacket* packet = nullptr;
    AVFrame* decodedFrame = nullptr;

    SwsContext* scaler = nullptr;

    int videoStreamIndex = -1;

    AVRational streamTimeBase{0, 1};
    AVRational frameRate{30, 1};

    bool demuxFinished = false;
    bool flushSent = false;
    bool opened = false;

    std::int64_t frameIndex = 0;

    void release() noexcept
    {
        sws_freeContext(scaler);
        scaler = nullptr;

        av_frame_free(&decodedFrame);
        av_packet_free(&packet);
        avcodec_free_context(&codecContext);

        if (formatContext)
        {
            avformat_close_input(&formatContext);
        }

        decoder = nullptr;
        videoStreamIndex = -1;

        demuxFinished = false;
        flushSent = false;
        opened = false;
        frameIndex = 0;
    }
};

namespace
{

std::string ffmpegError(int errorCode)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};

    av_strerror(errorCode, buffer, sizeof(buffer));

    return std::string(buffer);
}

bool validFrameRate(AVRational rate)
{
    return rate.num > 0 && rate.den > 0;
}

} // namespace

FileInput::FileInput(const std::string& filePath)
    : impl_(std::make_unique<Impl>()),
      filePath_(filePath)
{
}

FileInput::~FileInput()
{
    close();
}

bool FileInput::open()
{
    close();
    lastError_.clear();

    if (filePath_.empty())
    {
        lastError_ = "Input file path is empty.";
        return false;
    }

    Impl& state = *impl_;

    int result = avformat_open_input(
        &state.formatContext,
        filePath_.c_str(),
        nullptr,
        nullptr
    );

    if (result < 0)
    {
        lastError_ =
            "Could not open input file: " + ffmpegError(result);
        close();
        return false;
    }

    result = avformat_find_stream_info(
        state.formatContext,
        nullptr
    );

    if (result < 0)
    {
        lastError_ =
            "Could not read stream information: " +
            ffmpegError(result);

        close();
        return false;
    }

    result = av_find_best_stream(
        state.formatContext,
        AVMEDIA_TYPE_VIDEO,
        -1,
        -1,
        &state.decoder,
        0
    );

    if (result < 0)
    {
        lastError_ =
            "No usable video stream found: " +
            ffmpegError(result);

        close();
        return false;
    }

    state.videoStreamIndex = result;

    AVStream* videoStream =
        state.formatContext->streams[state.videoStreamIndex];

    state.streamTimeBase = videoStream->time_base;

    state.codecContext = avcodec_alloc_context3(state.decoder);

    if (!state.codecContext)
    {
        lastError_ = "Could not allocate decoder context.";
        close();
        return false;
    }

    result = avcodec_parameters_to_context(
        state.codecContext,
        videoStream->codecpar
    );

    if (result < 0)
    {
        lastError_ =
            "Could not copy codec parameters: " +
            ffmpegError(result);

        close();
        return false;
    }

    state.codecContext->pkt_timebase = videoStream->time_base;

    result = avcodec_open2(
        state.codecContext,
        state.decoder,
        nullptr
    );

    if (result < 0)
    {
        lastError_ =
            "Could not open video decoder: " +
            ffmpegError(result);

        close();
        return false;
    }

    AVRational detectedRate =
        av_guess_frame_rate(
            state.formatContext,
            videoStream,
            nullptr
        );

    if (!validFrameRate(detectedRate))
    {
        detectedRate = videoStream->avg_frame_rate;
    }

    if (!validFrameRate(detectedRate))
    {
        detectedRate = AVRational{30, 1};
    }

    state.frameRate = detectedRate;

    state.packet = av_packet_alloc();
    state.decodedFrame = av_frame_alloc();

    if (!state.packet || !state.decodedFrame)
    {
        lastError_ = "Could not allocate FFmpeg packet/frame.";
        close();
        return false;
    }

    state.demuxFinished = false;
    state.flushSent = false;
    state.frameIndex = 0;
    state.opened = true;

    return true;
}

VideoIOStatus FileInput::read(VideoFrame& outputFrame)
{
    Impl& state = *impl_;

    if (!state.opened)
    {
        lastError_ = "FileInput is not open.";
        return VideoIOStatus::Error;
    }

    for (;;)
    {
        // First, try to receive an already decoded frame.
        int result = avcodec_receive_frame(
            state.codecContext,
            state.decodedFrame
        );

        if (result == 0)
        {
            AVFrame* source = state.decodedFrame;

            const int width = source->width;
            const int height = source->height;

            // VideoFrame currently supports even-sized YUV420P.
            if (width <= 0 || height <= 0 ||
                width % 2 != 0 || height % 2 != 0)
            {
                lastError_ =
                    "Decoded frame has unsupported dimensions.";

                av_frame_unref(state.decodedFrame);
                return VideoIOStatus::Error;
            }

            VideoFormat outputFormat;

            outputFormat.width = width;
            outputFormat.height = height;
            outputFormat.pixelFormat = PixelFormat::YUV420P;
            outputFormat.frameRateNumerator = state.frameRate.num;
            outputFormat.frameRateDenominator = state.frameRate.den;

            if (!outputFrame.allocate(outputFormat))
            {
                lastError_ = "Could not allocate output video frame.";

                av_frame_unref(state.decodedFrame);
                return VideoIOStatus::Error;
            }

            state.scaler = sws_getCachedContext(
                state.scaler,
                width,
                height,
                static_cast<AVPixelFormat>(source->format),
                width,
                height,
                AV_PIX_FMT_YUV420P,
                SWS_BILINEAR,
                nullptr,
                nullptr,
                nullptr
            );

            if (!state.scaler)
            {
                lastError_ = "Could not create pixel format converter.";

                av_frame_unref(state.decodedFrame);
                return VideoIOStatus::Error;
            }

            std::uint8_t* destinationData[4] =
            {
                outputFrame.data.data() +
                    outputFrame.planes[0].offset,

                outputFrame.data.data() +
                    outputFrame.planes[1].offset,

                outputFrame.data.data() +
                    outputFrame.planes[2].offset,

                nullptr
            };

            int destinationLinesize[4] =
            {
                outputFrame.planes[0].stride,
                outputFrame.planes[1].stride,
                outputFrame.planes[2].stride,
                0
            };

            const int convertedRows = sws_scale(
                state.scaler,
                source->data,
                source->linesize,
                0,
                height,
                destinationData,
                destinationLinesize
            );

            if (convertedRows != height)
            {
                lastError_ = "Pixel format conversion failed.";

                av_frame_unref(state.decodedFrame);
                return VideoIOStatus::Error;
            }

            // Store presentation timestamps in microseconds.
            if (source->best_effort_timestamp != AV_NOPTS_VALUE)
            {
                outputFrame.presentationTimestamp =
                    av_rescale_q(
                        source->best_effort_timestamp,
                        state.streamTimeBase,
                        AV_TIME_BASE_Q
                    );
            }
            else
            {
                outputFrame.presentationTimestamp =
                    av_rescale_q(
                        state.frameIndex,
                        av_inv_q(state.frameRate),
                        AV_TIME_BASE_Q
                    );
            }

            outputFrame.duration =
                av_rescale_q(
                    1,
                    av_inv_q(state.frameRate),
                    AV_TIME_BASE_Q
                );

            ++state.frameIndex;

            av_frame_unref(state.decodedFrame);

            if (!outputFrame.isValid())
            {
                lastError_ = "Generated VideoFrame failed validation.";
                return VideoIOStatus::Error;
            }

            return VideoIOStatus::Ok;
        }

        if (result == AVERROR_EOF)
        {
            return VideoIOStatus::EndOfStream;
        }

        if (result != AVERROR(EAGAIN))
        {
            lastError_ =
                "Video decoding failed: " + ffmpegError(result);

            return VideoIOStatus::Error;
        }

        // Decoder needs more packets.
        if (state.demuxFinished)
        {
            if (state.flushSent)
            {
                return VideoIOStatus::EndOfStream;
            }

            result = avcodec_send_packet(
                state.codecContext,
                nullptr
            );

            if (result < 0 && result != AVERROR_EOF)
            {
                lastError_ =
                    "Decoder flush failed: " + ffmpegError(result);

                return VideoIOStatus::Error;
            }

            state.flushSent = true;
            continue;
        }

        // Read packets until a video packet is found.
        bool packetSubmitted = false;

        while (!packetSubmitted)
        {
            result = av_read_frame(
                state.formatContext,
                state.packet
            );

            if (result == AVERROR_EOF)
            {
                state.demuxFinished = true;
                break;
            }

            if (result < 0)
            {
                lastError_ =
                    "Input packet reading failed: " +
                    ffmpegError(result);

                return VideoIOStatus::Error;
            }

            if (state.packet->stream_index ==
                state.videoStreamIndex)
            {
                result = avcodec_send_packet(
                    state.codecContext,
                    state.packet
                );

                av_packet_unref(state.packet);

                if (result < 0)
                {
                    lastError_ =
                        "Could not submit packet to decoder: " +
                        ffmpegError(result);

                    return VideoIOStatus::Error;
                }

                packetSubmitted = true;
            }
            else
            {
                av_packet_unref(state.packet);
            }
        }
    }
}

void FileInput::close() noexcept
{
    if (impl_)
    {
        impl_->release();
    }
}

bool FileInput::isOpen() const noexcept
{
    return impl_ && impl_->opened;
}

const std::string& FileInput::getLastError() const noexcept
{
    return lastError_;
}

}
