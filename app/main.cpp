#include "FileInput.h"
#include "RTPInput.h"
#include "RTPOutput.h"
#include "D3D11Renderer.h"
#include "VideoStatistics.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

namespace
{

std::atomic<bool> g_consoleStopRequested{false};

BOOL WINAPI consoleControlHandler(DWORD controlType)
{
    if (controlType == CTRL_C_EVENT ||
        controlType == CTRL_BREAK_EVENT ||
        controlType == CTRL_CLOSE_EVENT)
    {
        g_consoleStopRequested.store(true);
        return TRUE;
    }

    return FALSE;
}

void printRate(const char* label, std::uint64_t count, double seconds)
{
    const double fps = seconds > 0.0
        ? static_cast<double>(count) / seconds
        : 0.0;

    std::cout << std::left << std::setw(38) << label
              << std::right << std::setw(10) << count
              << "  (" << std::fixed << std::setprecision(2)
              << fps << "/s)\n";
}

int runSender(int argc, char* argv[])
{
    if (argc < 3 || argc > 5)
    {
        std::cerr << "Usage: uvtf.exe --send <input-file> [destination-ip] [port]\n";
        return 1;
    }

    const std::string inputPath = argv[2];
    const std::string destinationIp = argc >= 4 ? argv[3] : "127.0.0.1";
    const std::string port = argc >= 5 ? argv[4] : "50000";
    const std::string destinationUrl =
        "rtp://" + destinationIp + ":" + port + "?pkt_size=1200";

    std::cout << "FFmpeg UVTF - RTP Video Sender\n"
              << "Input: " << inputPath << "\n"
              << "Destination: " << destinationUrl << "\n";

    uvtf::FileInput input(inputPath);
    if (!input.open())
    {
        std::cerr << "Input error: " << input.getLastError() << "\n";
        return 1;
    }

    uvtf::VideoFrame frame;
    auto status = input.read(frame);
    if (status != uvtf::VideoIOStatus::Ok)
    {
        std::cerr << "Could not decode first frame: "
                  << input.getLastError() << "\n";
        input.close();
        return 1;
    }

    const uvtf::VideoFormat format = frame.format;
    std::cout << "Input: " << format.width << "x" << format.height
              << " YUV420P, " << format.frameRateNumerator << "/"
              << format.frameRateDenominator << " fps\n";

    uvtf::RTPOutput output(format, destinationUrl, "uvtf_sender.sdp", 4000000);
    if (!output.open())
    {
        std::cerr << "RTP output error: " << output.getLastError() << "\n";
        input.close();
        return 1;
    }

    uvtf::VideoStatistics statistics;
    statistics.start();
    statistics.inputFrames = 1;

    std::cout << "SDP: uvtf_sender.sdp\nStarting transmission...\n";

    bool failed = false;
    const std::int64_t firstPts = frame.presentationTimestamp;
    const auto playbackStart = std::chrono::steady_clock::now();

    for (;;)
    {
        const auto elapsedUs = std::max<std::int64_t>(
            0, frame.presentationTimestamp - firstPts);
        std::this_thread::sleep_until(
            playbackStart + std::chrono::microseconds(elapsedUs));

        status = output.write(frame);
        if (status != uvtf::VideoIOStatus::Ok)
        {
            std::cerr << "\nRTP transmission error: "
                      << output.getLastError() << "\n";
            failed = true;
            break;
        }

        if (statistics.inputFrames % 100 == 0)
            std::cout << "\rFrames submitted: "
                      << output.getSubmittedFrameCount() << std::flush;

        status = input.read(frame);
        if (status == uvtf::VideoIOStatus::EndOfStream)
            break;
        if (status != uvtf::VideoIOStatus::Ok)
        {
            std::cerr << "\nInput decoding error: "
                      << input.getLastError() << "\n";
            failed = true;
            break;
        }

        ++statistics.inputFrames;
    }

    // RTPOutput::close() flushes the encoder and writes the RTP trailer.
    output.close();
    input.close();

    statistics.submittedFrames = output.getSubmittedFrameCount();
    statistics.encodedPackets = output.getEncodedPacketCount();
    statistics.stop();

    const double seconds = statistics.elapsedSeconds();

    std::cout << "\n\nUVTF SENDER STATISTICS\n"
              << "======================\n";
    std::cout << "Elapsed time                         "
              << std::fixed << std::setprecision(3) << seconds << " s\n";
    printRate("Input frames decoded", statistics.inputFrames, seconds);
    printRate("Frames submitted to encoder", statistics.submittedFrames, seconds);
    printRate("Encoded packets written to RTP muxer",
              statistics.encodedPackets, seconds);
    std::cout << "Destination: " << destinationUrl << "\n";
    std::cout << "SDP file: uvtf_sender.sdp\n";
    std::cout << "Sender result: "
              << (failed ? "FAILED" : "COMPLETED") << "\n";

    return failed || statistics.submittedFrames == 0 ? 1 : 0;
}

int runReceiver(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: uvtf.exe --receive <sdp-file>\n";
        return 1;
    }

    SetConsoleCtrlHandler(consoleControlHandler, TRUE);
    g_consoleStopRequested.store(false);

    const std::string sdpPath = argv[2];
    std::cout << "FFmpeg UVTF - RTP Video Receiver\n"
              << "SDP: " << sdpPath << "\n"
              << "Waiting for RTP/H.264 packets...\n";

    uvtf::RTPInput input(sdpPath);
    input.setIdleTimeoutMs(2000);

    if (!input.open())
    {
        std::cerr << "RTP input error: " << input.getLastError() << "\n";
        SetConsoleCtrlHandler(consoleControlHandler, FALSE);
        return 1;
    }

    uvtf::VideoStatistics statistics;
    statistics.start();

    std::cout << "RTP input opened. Waiting for decoded video frames...\n";

    uvtf::D3D11Renderer renderer;
    bool rendererOpened = false;
    bool stopRequested = false;
    bool failed = false;
    std::uint64_t decodedAfterStop = 0;
    auto lastReport = std::chrono::steady_clock::now();

    for (;;)
    {
        if (!stopRequested && g_consoleStopRequested.load())
        {
            stopRequested = true;
            std::cout << "\nShutdown requested from console. Draining decoder...\n";
            input.requestStop();
        }

        if (!stopRequested && rendererOpened && !renderer.pumpMessages())
        {
            stopRequested = true;
            std::cout << "\nVideo window close requested. Draining decoder...\n";
            input.requestStop();
        }

        uvtf::VideoFrame frame;
        const auto status = input.read(frame);

        if (status == uvtf::VideoIOStatus::WouldBlock)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        if (status == uvtf::VideoIOStatus::EndOfStream)
        {
            std::cout << "\nRTP stream ended; decoder drain completed.\n";
            break;
        }

        if (status == uvtf::VideoIOStatus::Error)
        {
            std::cerr << "\nRTP receive/decode error: "
                      << input.getLastError() << "\n";
            failed = true;
            break;
        }

        if (!rendererOpened)
        {
            std::cout << "Decoded format: " << frame.format.width << "x"
                      << frame.format.height << " BGRA32\n";

            if (!renderer.open(frame.format))
            {
                std::cerr << "D3D11 initialization error: "
                          << renderer.getLastError() << "\n";
                failed = true;
                break;
            }

            rendererOpened = true;
            std::cout << "Direct3D 11 window opened.\n";
        }

        // Once the user explicitly closes the display, continue draining
        // the decoder but do not present additional frames to a closing UI.
        if (stopRequested)
        {
            ++decodedAfterStop;
            continue;
        }

        if (!renderer.render(frame))
        {
            std::cerr << "D3D11 rendering error: "
                      << renderer.getLastError() << "\n";
            ++statistics.renderErrors;
            failed = true;
            input.requestStop();
            stopRequested = true;
            continue;
        }

        ++statistics.renderedFrames;

        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport >= std::chrono::seconds(1))
        {
            std::cout << "\rFrames rendered: "
                      << statistics.renderedFrames << std::flush;
            lastReport = now;
        }
    }

    // Capture counters before closing RTPInput. Its close operation releases
    // FFmpeg resources but deliberately preserves the final counter values.
    statistics.demuxedVideoPackets = input.getDemuxedVideoPacketCount();
    statistics.decodedFrames = input.getDecodedFrameCount();
    statistics.decoderErrors = input.getDecoderErrorCount();

    input.close();
    renderer.close();
    statistics.stop();
    SetConsoleCtrlHandler(consoleControlHandler, FALSE);

    const double seconds = statistics.elapsedSeconds();

    std::cout << "\n\nUVTF RECEIVER STATISTICS\n"
              << "========================\n";
    std::cout << "Elapsed time                         "
              << std::fixed << std::setprecision(3) << seconds << " s\n";
    printRate("Demuxed video packets",
              statistics.demuxedVideoPackets, seconds);
    printRate("H.264 frames decoded",
              statistics.decodedFrames, seconds);
    printRate("Frames rendered by D3D11",
              statistics.renderedFrames, seconds);
    std::cout << "Decoded after user stop (not rendered) "
              << decodedAfterStop << "\n";
    std::cout << "Decoder errors                       "
              << statistics.decoderErrors << "\n";
    std::cout << "Renderer errors                      "
              << statistics.renderErrors << "\n";
    std::cout << "Note: demuxed video packets are FFmpeg depacketized packets, "
                 "not raw UDP datagrams.\n";
    std::cout << "Receiver result: "
              << (failed ? "FAILED" : "STOPPED CLEANLY") << "\n";

    return failed ? 1 : 0;
}

}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cout
            << "FFmpeg Universal Video Transport Framework\n\n"
            << "Modes:\n"
            << "  uvtf.exe --send <input-file> [destination-ip] [port]\n"
            << "  uvtf.exe --receive <sdp-file>\n\n"
            << "Examples:\n"
            << "  uvtf.exe --send sample.mp4 127.0.0.1 50000\n"
            << "  uvtf.exe --receive uvtf_sender.sdp\n";
        return 1;
    }

    const std::string mode = argv[1];
    if (mode == "--send")
        return runSender(argc, argv);
    if (mode == "--receive")
        return runReceiver(argc, argv);

    if (!mode.empty() && mode.rfind("--", 0) != 0)
    {
        char* legacyArgv[6]{};
        legacyArgv[0] = argv[0];
        legacyArgv[1] = const_cast<char*>("--send");
        for (int i = 1; i < argc; ++i)
            legacyArgv[i + 1] = argv[i];
        return runSender(argc + 1, legacyArgv);
    }

    std::cerr << "Unknown mode: " << mode << "\n";
    return 1;
}
