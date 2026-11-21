
#include "RTPOutput.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <chrono>

namespace uvtf
{

struct RTPOutput::Impl
{
    AVFormatContext* formatContext = nullptr;
    AVCodecContext* encoderContext = nullptr;
    const AVCodec* encoder = nullptr;

    AVStream* stream = nullptr;
    AVFrame* encoderFrame = nullptr;
    AVPacket* packet = nullptr;

    bool opened = false;
    bool headerWritten = false;

    std::int64_t lastPts = AV_NOPTS_VALUE;
    std::uint64_t submittedFrames = 0;
    std::uint64_t encodedPacketsWritten = 0;

    void release() noexcept
    {
        av_packet_free(&packet);
        av_frame_free(&encoderFrame);
        avcodec_free_context(&encoderContext);

        if (formatContext)
        {
            if (!(formatContext->oformat->flags & AVFMT_NOFILE) &&
                formatContext->pb)
            {
                avio_closep(&formatContext->pb);
            }

            avformat_free_context(formatContext);
            formatContext = nullptr;
        }

        encoder = nullptr;
        stream = nullptr;

        opened = false;
        headerWritten = false;
        lastPts = AV_NOPTS_VALUE;
    }
};

namespace
{

void logStage(const char* message)
{
    std::fprintf(stderr, "[RTPOutput] %s\n", message);
    std::fflush(stderr);
}

std::string ffmpegError(int errorCode)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};

    av_strerror(errorCode, buffer, sizeof(buffer));

    return std::string(buffer);
}

bool parseDestination(
    const std::string& url,
    std::string& host,
    int& port)
{
    constexpr const char* prefix = "rtp://";

    if (url.compare(0, 6, prefix) != 0)
    {
        return false;
    }

    const std::size_t start = 6;

    const std::size_t queryPosition = url.find('?', start);

    const std::string address = url.substr(
        start,
        queryPosition == std::string::npos
            ? std::string::npos
            : queryPosition - start
    );

    const std::size_t colon = address.rfind(':');

    if (colon == std::string::npos)
    {
        return false;
    }

    host = address.substr(0, colon);

    if (host.empty())
    {
        return false;
    }

    try
    {
        const std::string portText = address.substr(colon + 1);

        std::size_t parsed = 0;

        const unsigned long value =
            std::stoul(portText, &parsed);

        if (parsed != portText.size() ||
            value == 0 ||
            value > 65535)
        {
            return false;
        }

        port = static_cast<int>(value);
    }
    catch (...)
    {
        return false;
    }

    return true;
}

bool writeSdpFile(
    const std::string& filePath,
    const std::string& destinationUrl,
    std::string& error)
{
    std::string host;
    int port = 0;

    if (!parseDestination(destinationUrl, host, port))
    {
        error = "Could not parse RTP destination URL for SDP.";
        return false;
    }

    std::ofstream file(
        filePath,
        std::ios::out | std::ios::trunc
    );

    if (!file)
    {
        error = "Could not create SDP file: " + filePath;
        return false;
    }

    file
        << "v=0\r\n"
        << "o=- 0 0 IN IP4 " << host << "\r\n"
        << "s=UVTF H264 RTP Stream\r\n"
        << "c=IN IP4 " << host << "\r\n"
        << "t=0 0\r\n"
        << "m=video " << port << " RTP/AVP 96\r\n"
        << "a=rtpmap:96 H264/90000\r\n"
        << "a=fmtp:96 packetization-mode=1\r\n"
        << "a=recvonly\r\n";

    file.close();

    if (!file)
    {
        error = "Could not finish writing SDP file.";
        return false;
    }

    return true;
}

// Drain all currently available encoded packets.
template <typename State>
bool drainEncoder(
    State& state,
    std::string& error)
{
    for (;;)
    {
        const int result = avcodec_receive_packet(
            state.encoderContext,
            state.packet
        );

        if (result == AVERROR(EAGAIN) ||
            result == AVERROR_EOF)
        {
            return true;
        }

        if (result < 0)
        {
            error =
                "Could not receive encoded packet: " +
                ffmpegError(result);

            return false;
        }

        av_packet_rescale_ts(
            state.packet,
            state.encoderContext->time_base,
            state.stream->time_base
        );

        state.packet->stream_index = state.stream->index;
        state.packet->pos = -1;

        const int writeResult = av_write_frame(
            state.formatContext,
            state.packet
        );

        av_packet_unref(state.packet);

        if (writeResult < 0)
        {
            error =
                "RTP packet transmission failed: " +
                ffmpegError(writeResult);

            return false;
        }

        ++state.encodedPacketsWritten;
    }
}

} // namespace

RTPOutput::RTPOutput(
    const VideoFormat& format,
    const std::string& destinationUrl,
    const std::string& sdpFilePath,
    int bitRate
)
    : impl_(std::make_unique<Impl>()),
      format_(format),
      destinationUrl_(destinationUrl),
      sdpFilePath_(sdpFilePath),
      bitRate_(bitRate)
{
}

RTPOutput::~RTPOutput()
{
    close();
}

bool RTPOutput::open()
{
    close();
    lastError_.clear();

    Impl& state = *impl_;
    state.submittedFrames = 0;
    state.encodedPacketsWritten = 0;

    logStage("Starting RTP initialization.");

    if (!format_.isValid() ||
        format_.pixelFormat != PixelFormat::YUV420P ||
        format_.width % 2 != 0 ||
        format_.height % 2 != 0)
    {
        lastError_ =
            "RTP output requires valid even-sized YUV420P video.";

        return false;
    }

    if (destinationUrl_.empty() || bitRate_ <= 0)
    {
        lastError_ = "Invalid destination URL or bitrate.";
        return false;
    }

    logStage("Initializing FFmpeg network support.");

    const int networkResult = avformat_network_init();

    if (networkResult < 0)
    {
        lastError_ =
            "FFmpeg network initialization failed: " +
            ffmpegError(networkResult);

        return false;
    }

    logStage("Finding H.264 encoder.");

    state.encoder = avcodec_find_encoder_by_name("libx264");

    if (!state.encoder)
    {
        state.encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    }

    if (!state.encoder)
    {
        lastError_ = "No H.264 encoder is available.";
        return false;
    }

    logStage("Creating RTP output context.");

    int result = avformat_alloc_output_context2(
        &state.formatContext,
        nullptr,
        "rtp",
        destinationUrl_.c_str()
    );

    if (result < 0 || !state.formatContext)
    {
        lastError_ =
            "Could not create RTP context: " +
            ffmpegError(result);

        close();
        return false;
    }

    logStage("Allocating H.264 encoder context.");

    state.encoderContext = avcodec_alloc_context3(state.encoder);

    if (!state.encoderContext)
    {
        lastError_ = "Could not allocate H.264 encoder context.";
        close();
        return false;
    }

    AVCodecContext* encoder = state.encoderContext;

    encoder->codec_id = AV_CODEC_ID_H264;
    encoder->codec_type = AVMEDIA_TYPE_VIDEO;

    encoder->width = format_.width;
    encoder->height = format_.height;

    encoder->pix_fmt = AV_PIX_FMT_YUV420P;

    encoder->time_base = AVRational{
        format_.frameRateDenominator,
        format_.frameRateNumerator
    };

    encoder->framerate = AVRational{
        format_.frameRateNumerator,
        format_.frameRateDenominator
    };

    encoder->bit_rate = bitRate_;

    encoder->gop_size = std::max(
        1,
        format_.frameRateNumerator /
        format_.frameRateDenominator * 2
    );

    encoder->max_b_frames = 0;
    encoder->thread_count = 0;

    // Let the RTP stream carry the parameter sets in-band.
    // This supports the manually generated SDP description.
    AVDictionary* encoderOptions = nullptr;

    if (std::string(state.encoder->name) == "libx264")
    {
        av_dict_set(&encoderOptions, "preset", "ultrafast", 0);
        av_dict_set(&encoderOptions, "tune", "zerolatency", 0);
        av_dict_set(
            &encoderOptions,
            "x264-params",
            "repeat-headers=1",
            0
        );
    }

    logStage("Opening H.264 encoder.");

    result = avcodec_open2(
        encoder,
        state.encoder,
        &encoderOptions
    );

    av_dict_free(&encoderOptions);

    if (result < 0)
    {
        lastError_ =
            "Could not open H.264 encoder: " +
            ffmpegError(result);

        close();
        return false;
    }

    logStage("H.264 encoder opened successfully.");

    logStage("Creating RTP video stream.");

    state.stream = avformat_new_stream(
        state.formatContext,
        nullptr
    );

    if (!state.stream)
    {
        lastError_ = "Could not create RTP video stream.";
        close();
        return false;
    }

    state.stream->id = 96;
    state.stream->time_base = encoder->time_base;
    state.stream->avg_frame_rate = encoder->framerate;

    result = avcodec_parameters_from_context(
        state.stream->codecpar,
        encoder
    );

    if (result < 0)
    {
        lastError_ =
            "Could not copy encoder parameters: " +
            ffmpegError(result);

        close();
        return false;
    }

    logStage("Allocating encoder frame and packet.");

    state.encoderFrame = av_frame_alloc();
    state.packet = av_packet_alloc();

    if (!state.encoderFrame || !state.packet)
    {
        lastError_ = "Could not allocate encoder frame or packet.";
        close();
        return false;
    }

    state.encoderFrame->format = encoder->pix_fmt;
    state.encoderFrame->width = encoder->width;
    state.encoderFrame->height = encoder->height;

    result = av_frame_get_buffer(state.encoderFrame, 32);

    if (result < 0)
    {
        lastError_ =
            "Could not allocate encoder frame buffer: " +
            ffmpegError(result);

        close();
        return false;
    }

    logStage("Checking RTP output I/O configuration.");

    // RTP normally uses AVFMT_NOFILE and manages its own UDP sockets.
    // Only open AVIO explicitly when the selected muxer requires it.
    if (!(state.formatContext->oformat->flags & AVFMT_NOFILE))
    {
        result = avio_open(
            &state.formatContext->pb,
            destinationUrl_.c_str(),
            AVIO_FLAG_WRITE
        );

        if (result < 0)
        {
            lastError_ =
                "Could not open RTP output I/O: " +
                ffmpegError(result);

            close();
            return false;
        }
    }

    logStage("Writing RTP muxer header.");

    result = avformat_write_header(
        state.formatContext,
        nullptr
    );

    if (result < 0)
    {
        lastError_ =
            "Could not initialize RTP muxer: " +
            ffmpegError(result);

        close();
        return false;
    }

    state.headerWritten = true;

    logStage("RTP muxer header written successfully.");

    logStage("Generating SDP description.");

    if (!writeSdpFile(
            sdpFilePath_,
            destinationUrl_,
            lastError_))
    {
        close();
        return false;
    }

    state.opened = true;

    logStage("RTP output initialization completed.");

    return true;
}

VideoIOStatus RTPOutput::write(const VideoFrame& inputFrame)
{
    Impl& state = *impl_;

    if (!state.opened)
    {
        lastError_ = "RTP output is not open.";
        return VideoIOStatus::Error;
    }

    if (!inputFrame.isValid() ||
        inputFrame.format.width != format_.width ||
        inputFrame.format.height != format_.height ||
        inputFrame.format.pixelFormat != PixelFormat::YUV420P ||
        inputFrame.planeCount < 3)
    {
        lastError_ = "Input frame does not match RTP format.";
        return VideoIOStatus::Error;
    }

    int result = av_frame_make_writable(state.encoderFrame);

    if (result < 0)
    {
        lastError_ =
            "Encoder frame is not writable: " +
            ffmpegError(result);

        return VideoIOStatus::Error;
    }

    // Copy Y, U and V planes into the FFmpeg frame.
    for (int planeIndex = 0; planeIndex < 3; ++planeIndex)
    {
        const VideoPlane& sourcePlane =
            inputFrame.planes[planeIndex];

        const int rowBytes = sourcePlane.width;
        const int rows = sourcePlane.height;

        const std::uint8_t* source =
            inputFrame.data.data() + sourcePlane.offset;

        std::uint8_t* destination =
            state.encoderFrame->data[planeIndex];

        const int destinationStride =
            state.encoderFrame->linesize[planeIndex];

        if (sourcePlane.stride < rowBytes ||
            destinationStride < rowBytes ||
            rowBytes <= 0 ||
            rows <= 0)
        {
            lastError_ = "Invalid video plane dimensions or stride.";
            return VideoIOStatus::Error;
        }

        for (int row = 0; row < rows; ++row)
        {
            std::memcpy(
                destination + row * destinationStride,
                source + row * sourcePlane.stride,
                static_cast<std::size_t>(rowBytes)
            );
        }
    }

    const AVRational microsecondTimeBase{1, AV_TIME_BASE};

    const std::int64_t pts = av_rescale_q(
        inputFrame.presentationTimestamp,
        microsecondTimeBase,
        state.encoderContext->time_base
    );

    if (state.lastPts != AV_NOPTS_VALUE && pts <= state.lastPts)
    {
        lastError_ = "Input presentation timestamps are not increasing.";
        return VideoIOStatus::Error;
    }

    state.encoderFrame->pts = pts;

    logStage("Submitting frame to H.264 encoder.");

    result = avcodec_send_frame(
        state.encoderContext,
        state.encoderFrame
    );

    if (result == AVERROR(EAGAIN))
    {
        if (!drainEncoder(state, lastError_))
        {
            return VideoIOStatus::Error;
        }

        result = avcodec_send_frame(
            state.encoderContext,
            state.encoderFrame
        );
    }

    if (result < 0)
    {
        lastError_ =
            "H.264 encoding failed: " +
            ffmpegError(result);

        return VideoIOStatus::Error;
    }

    state.lastPts = pts;
    ++state.submittedFrames;

    if (!drainEncoder(state, lastError_))
    {
        return VideoIOStatus::Error;
    }

    return VideoIOStatus::Ok;
}

void RTPOutput::close() noexcept
{
    if (!impl_)
    {
        return;
    }

    using Clock = std::chrono::steady_clock;

    const auto totalStart = Clock::now();

    auto logElapsed = [](const char* stage,
                         Clock::time_point start)
    {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - start
            ).count();

        std::fprintf(
            stderr,
            "[RTPOutput Timing] %s: %lld ms\n",
            stage,
            static_cast<long long>(elapsed)
        );

        std::fflush(stderr);
    };

    Impl& state = *impl_;

    if (state.headerWritten &&
        state.encoderContext &&
        state.formatContext)
    {
        if (state.opened)
        {
            logStage("Flushing H.264 encoder.");

            const auto flushStart = Clock::now();

            int result = avcodec_send_frame(
                state.encoderContext,
                nullptr
            );

            if (result == AVERROR(EAGAIN))
            {
                std::string ignoredError;

                drainEncoder(state, ignoredError);

                result = avcodec_send_frame(
                    state.encoderContext,
                    nullptr
                );
            }

            if (result >= 0 || result == AVERROR_EOF)
            {
                std::string ignoredError;
                drainEncoder(state, ignoredError);
            }

            logElapsed("Encoder flush", flushStart);
        }

        logStage("Writing RTP trailer.");

        const auto trailerStart = Clock::now();

        const int trailerResult =
            av_write_trailer(state.formatContext);

        logElapsed("RTP trailer", trailerStart);

        if (trailerResult < 0)
        {
            std::fprintf(
                stderr,
                "[RTPOutput] Trailer error: %s\n",
                ffmpegError(trailerResult).c_str()
            );
        }
    }

    const auto releaseStart = Clock::now();

    state.release();

    logElapsed("Resource release", releaseStart);
    logElapsed("Total close()", totalStart);
}

bool RTPOutput::isOpen() const noexcept
{
    return impl_ && impl_->opened;
}

const std::string& RTPOutput::getLastError() const noexcept
{
    return lastError_;
}

std::uint64_t RTPOutput::getSubmittedFrameCount() const noexcept
{
    return impl_ ? impl_->submittedFrames : 0;
}

std::uint64_t RTPOutput::getEncodedPacketCount() const noexcept
{
    return impl_ ? impl_->encodedPacketsWritten : 0;
}

}
