#include "RTPInput.h"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cstdint>
#include <string>

namespace uvtf
{

struct RTPInput::Impl
{
    using Clock = std::chrono::steady_clock;

    AVFormatContext* formatContext = nullptr;
    AVCodecContext* decoderContext = nullptr;
    const AVCodec* decoder = nullptr;
    AVFrame* decodedFrame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scaler = nullptr;

    int videoStreamIndex = -1;
    AVRational streamTimeBase{0, 1};
    AVRational frameRate{30, 1};

    bool opened = false;
    bool demuxEof = false;
    bool decoderFlushSent = false;
    bool decoderEof = false;
    bool stopRequested = false;
    bool receivedFirstVideoPacket = false;
    bool packetPending = false;

    std::uint32_t idleTimeoutMs = 500;
    Clock::time_point lastVideoPacketAt{};

    std::uint64_t demuxedVideoPackets = 0;
    std::uint64_t decodedFrames = 0;
    std::uint64_t decoderErrors = 0;

    void release() noexcept
    {
        sws_freeContext(scaler);
        scaler = nullptr;

        av_packet_free(&packet);
        av_frame_free(&decodedFrame);
        avcodec_free_context(&decoderContext);

        if (formatContext)
            avformat_close_input(&formatContext);

        decoder = nullptr;
        videoStreamIndex = -1;
        streamTimeBase = AVRational{0, 1};
        frameRate = AVRational{30, 1};

        opened = false;
        demuxEof = false;
        decoderFlushSent = false;
        decoderEof = false;
        stopRequested = false;
        receivedFirstVideoPacket = false;
        packetPending = false;
        lastVideoPacketAt = Clock::time_point{};
    }
};

namespace
{

std::string ffmpegError(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

bool copyDecodedFrame(
    RTPInput::Impl& state,
    VideoFrame& output,
    std::string& error)
{
    const AVFrame* source = state.decodedFrame;

    if (!source || source->width <= 0 || source->height <= 0)
    {
        error = "Decoder produced a frame with invalid dimensions.";
        return false;
    }

    VideoFormat format;
    format.width = source->width;
    format.height = source->height;
    format.pixelFormat = PixelFormat::BGRA32;
    format.frameRateNumerator =
        state.frameRate.num > 0 ? state.frameRate.num : 30;
    format.frameRateDenominator =
        state.frameRate.den > 0 ? state.frameRate.den : 1;

    if (!output.allocate(format))
    {
        error = "Could not allocate BGRA32 UVTF VideoFrame.";
        return false;
    }

    state.scaler = sws_getCachedContext(
        state.scaler,
        source->width,
        source->height,
        static_cast<AVPixelFormat>(source->format),
        source->width,
        source->height,
        AV_PIX_FMT_BGRA,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr
    );

    if (!state.scaler)
    {
        error = "Could not create FFmpeg pixel-format converter.";
        return false;
    }

    std::uint8_t* destinationData[4] = {
        output.data.data(), nullptr, nullptr, nullptr
    };
    int destinationLinesize[4] = {
        output.planes[0].stride, 0, 0, 0
    };

    const int rows = sws_scale(
        state.scaler,
        source->data,
        source->linesize,
        0,
        source->height,
        destinationData,
        destinationLinesize
    );

    if (rows != source->height)
    {
        error = "FFmpeg pixel conversion returned an incomplete frame.";
        return false;
    }

    std::int64_t timestamp = source->best_effort_timestamp;
    if (timestamp == AV_NOPTS_VALUE)
        timestamp = source->pts;

    output.presentationTimestamp =
        timestamp == AV_NOPTS_VALUE
            ? 0
            : av_rescale_q(
                timestamp,
                state.streamTimeBase,
                AVRational{1, AV_TIME_BASE}
              );

    if (source->duration > 0)
    {
        output.duration = av_rescale_q(
            source->duration,
            state.streamTimeBase,
            AVRational{1, AV_TIME_BASE}
        );
    }
    else
    {
        output.duration = av_rescale_q(
            1,
            av_inv_q(state.frameRate),
            AVRational{1, AV_TIME_BASE}
        );
    }

    return true;
}

} // namespace

RTPInput::RTPInput(const std::string& sdpFilePath)
    : impl_(std::make_unique<Impl>()),
      sdpFilePath_(sdpFilePath)
{
}

RTPInput::~RTPInput()
{
    close();
}

bool RTPInput::open()
{
    close();
    lastError_.clear();

    impl_->demuxedVideoPackets = 0;
    impl_->decodedFrames = 0;
    impl_->decoderErrors = 0;

    if (sdpFilePath_.empty())
    {
        lastError_ = "SDP file path is empty.";
        return false;
    }

    avformat_network_init();

    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file,udp,rtp", 0);
    // A short socket timeout lets the application process window messages
    // and evaluate the longer stream-idle timeout.
    av_dict_set(&options, "timeout", "250000", 0);

    int result = avformat_open_input(
        &impl_->formatContext,
        sdpFilePath_.c_str(),
        nullptr,
        &options
    );
    av_dict_free(&options);

    if (result < 0)
    {
        lastError_ = "Could not open SDP/RTP input: " + ffmpegError(result);
        close();
        return false;
    }

    result = avformat_find_stream_info(impl_->formatContext, nullptr);
    if (result < 0)
    {
        lastError_ = "Could not read RTP stream information: " + ffmpegError(result);
        close();
        return false;
    }

    for (unsigned int i = 0; i < impl_->formatContext->nb_streams; ++i)
    {
        AVStream* stream = impl_->formatContext->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            impl_->videoStreamIndex = static_cast<int>(i);
            break;
        }
    }

    if (impl_->videoStreamIndex < 0)
    {
        lastError_ = "The SDP input does not contain a video stream.";
        close();
        return false;
    }

    AVStream* stream =
        impl_->formatContext->streams[impl_->videoStreamIndex];

    impl_->streamTimeBase = stream->time_base;
    impl_->frameRate = stream->avg_frame_rate.num > 0
        ? stream->avg_frame_rate
        : stream->r_frame_rate;

    if (impl_->frameRate.num <= 0 || impl_->frameRate.den <= 0)
        impl_->frameRate = AVRational{30, 1};

    impl_->decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!impl_->decoder)
    {
        lastError_ = "No FFmpeg decoder is available for the RTP video codec.";
        close();
        return false;
    }

    impl_->decoderContext = avcodec_alloc_context3(impl_->decoder);
    if (!impl_->decoderContext)
    {
        lastError_ = "Could not allocate H.264 decoder context.";
        close();
        return false;
    }

    result = avcodec_parameters_to_context(
        impl_->decoderContext,
        stream->codecpar
    );
    if (result < 0)
    {
        lastError_ = "Could not copy RTP codec parameters: " + ffmpegError(result);
        close();
        return false;
    }

    impl_->decoderContext->thread_count = 0;

    result = avcodec_open2(
        impl_->decoderContext,
        impl_->decoder,
        nullptr
    );
    if (result < 0)
    {
        lastError_ = "Could not open H.264 decoder: " + ffmpegError(result);
        close();
        return false;
    }

    impl_->decodedFrame = av_frame_alloc();
    impl_->packet = av_packet_alloc();

    if (!impl_->decodedFrame || !impl_->packet)
    {
        lastError_ = "Could not allocate FFmpeg receive frame or packet.";
        close();
        return false;
    }

    impl_->opened = true;
    return true;
}

VideoIOStatus RTPInput::read(VideoFrame& output)
{
    Impl& state = *impl_;

    if (!state.opened)
    {
        lastError_ = "RTP input is not open.";
        return VideoIOStatus::Error;
    }

    for (;;)
    {
        // Drain decoded output before attempting to read another RTP packet.
        int result = avcodec_receive_frame(
            state.decoderContext,
            state.decodedFrame
        );

        if (result == 0)
        {
            const bool copied = copyDecodedFrame(state, output, lastError_);
            av_frame_unref(state.decodedFrame);

            if (copied)
                ++state.decodedFrames;
            else
                ++state.decoderErrors;

            return copied ? VideoIOStatus::Ok : VideoIOStatus::Error;
        }

        if (result == AVERROR_EOF)
        {
            state.decoderEof = true;
            return VideoIOStatus::EndOfStream;
        }

        if (result != AVERROR(EAGAIN))
        {
            ++state.decoderErrors;
            lastError_ = "H.264 decoder receive failed: " + ffmpegError(result);
            return VideoIOStatus::Error;
        }

        // If avcodec_send_packet previously returned EAGAIN, retain that
        // packet and retry it after receiving available decoder output.
        if (state.packetPending)
        {
            result = avcodec_send_packet(state.decoderContext, state.packet);

            if (result == AVERROR(EAGAIN))
            {
                ++state.decoderErrors;
                lastError_ =
                    "H.264 decoder remained in EAGAIN after receive_frame; "
                    "the pending RTP packet was not discarded.";
                return VideoIOStatus::Error;
            }

            av_packet_unref(state.packet);
            state.packetPending = false;

            if (result < 0)
            {
                ++state.decoderErrors;
                lastError_ =
                    "Could not submit pending RTP packet to H.264 decoder: " +
                    ffmpegError(result);
                return VideoIOStatus::Error;
            }

            continue;
        }

        // A user stop or stream-idle timeout initiates the same decoder drain.
        if (state.stopRequested)
            state.demuxEof = true;

        if (state.demuxEof)
        {
            if (!state.decoderFlushSent)
            {
                result = avcodec_send_packet(state.decoderContext, nullptr);

                if (result == AVERROR(EAGAIN))
                {
                    // The decoder asks us to receive output before accepting
                    // the flush marker. Return to receive_frame().
                    continue;
                }

                if (result < 0 && result != AVERROR_EOF)
                {
                    ++state.decoderErrors;
                    lastError_ = "Could not flush H.264 decoder: " + ffmpegError(result);
                    return VideoIOStatus::Error;
                }

                state.decoderFlushSent = true;
                continue;
            }

            // After a successful flush, FFmpeg should eventually return EOF.
            // EAGAIN here is not treated as a clean end-of-stream.
            ++state.decoderErrors;
            lastError_ = "Decoder requested more input after flush; drain did not complete.";
            return VideoIOStatus::Error;
        }

        result = av_read_frame(state.formatContext, state.packet);

        if (result == AVERROR(EAGAIN) ||
            result == AVERROR(ETIMEDOUT) ||
            result == AVERROR(EINTR))
        {
            if (state.receivedFirstVideoPacket &&
                state.idleTimeoutMs > 0)
            {
                const auto idleFor =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        Impl::Clock::now() - state.lastVideoPacketAt
                    ).count();

                if (idleFor >= state.idleTimeoutMs)
                {
                    state.demuxEof = true;
                    continue;
                }
            }

            return VideoIOStatus::WouldBlock;
        }

        if (result == AVERROR_EOF)
        {
            state.demuxEof = true;
            continue;
        }

        if (result < 0)
        {
            ++state.decoderErrors;
            lastError_ = "RTP packet reception failed: " + ffmpegError(result);
            return VideoIOStatus::Error;
        }

        if (state.packet->stream_index == state.videoStreamIndex)
        {
            ++state.demuxedVideoPackets;
            state.receivedFirstVideoPacket = true;
            state.lastVideoPacketAt = Impl::Clock::now();

            result = avcodec_send_packet(state.decoderContext, state.packet);

            if (result == AVERROR(EAGAIN))
            {
                // Keep the AVPacket referenced; read() will receive output
                // first and retry this exact packet on the next iteration.
                state.packetPending = true;
                continue;
            }

            av_packet_unref(state.packet);

            if (result < 0)
            {
                ++state.decoderErrors;
                lastError_ = "Could not submit RTP packet to H.264 decoder: " +
                    ffmpegError(result);
                return VideoIOStatus::Error;
            }
        }
        else
        {
            av_packet_unref(state.packet);
        }
    }
}

void RTPInput::requestStop() noexcept
{
    if (impl_ && impl_->opened)
        impl_->stopRequested = true;
}

bool RTPInput::isDraining() const noexcept
{
    return impl_ && impl_->demuxEof && impl_->decoderFlushSent &&
        !impl_->decoderEof;
}

void RTPInput::setIdleTimeoutMs(std::uint32_t timeoutMs) noexcept
{
    if (impl_)
        impl_->idleTimeoutMs = timeoutMs;
}

void RTPInput::close() noexcept
{
    if (impl_)
        impl_->release();
}

bool RTPInput::isOpen() const noexcept
{
    return impl_ && impl_->opened;
}

const std::string& RTPInput::getLastError() const noexcept
{
    return lastError_;
}

std::uint64_t RTPInput::getDemuxedVideoPacketCount() const noexcept
{
    return impl_ ? impl_->demuxedVideoPackets : 0;
}

std::uint64_t RTPInput::getDecodedFrameCount() const noexcept
{
    return impl_ ? impl_->decodedFrames : 0;
}

std::uint64_t RTPInput::getDecoderErrorCount() const noexcept
{
    return impl_ ? impl_->decoderErrors : 0;
}

}
