# FFmpeg Universal Video Transport Framework (UVTF)

A C++17 video transport framework built using FFmpeg, H.264, RTP/UDP, and Direct3D 11. UVTF provides a sender and receiver pipeline for encoding, transporting, decoding, and rendering video.

## Features

- File-based video input and decoding
- H.264 video encoding using FFmpeg and libx264
- RTP packetization and UDP transport
- SDP-based RTP stream configuration
- RTP reception and H.264 depacketization
- H.264 decoding at the receiver
- BGRA32 pixel-format conversion using libswscale
- Direct3D 11 video rendering on Windows
- Pipeline statistics and graceful sender shutdown
- Receiver shutdown with idle detection and decoder draining

## Architecture

```text
                 SENDER
                   |
             Input Video File
                   |
             FFmpeg Decoder
                   |
             Video Frames
                   |
             H.264 Encoder
                (libx264)
                   |
              RTP Muxer
                   |
              UDP Transport
                   |
             SDP Description
                   |
          ---------------------
                   |
                 NETWORK
                   |
          ---------------------
                   |
              UDP Transport
                   |
              RTP Demuxer
                   |
          H.264 Depacketization
                   |
             FFmpeg Decoder
                   |
             Video Frames
                   |
           Pixel Conversion
               (BGRA32)
                   |
             Direct3D 11
                   |
             Video Display
                 RECEIVER
```

## Technology Stack

| Component | Technology |
|---|---|
| Programming language | C++17 |
| Multimedia framework | FFmpeg |
| Video codec | H.264 |
| Encoder | libx264 |
| Transport | RTP over UDP |
| Stream description | SDP |
| Pixel conversion | libswscale |
| Rendering | Direct3D 11 |
| Build system | CMake |
| Dependency management | vcpkg |
| Compiler | MSVC |

## Requirements

- Windows 10 or Windows 11, 64-bit
- Visual Studio 2022 or later with the MSVC C++ desktop development tools
- CMake 3.20 or later
- Git
- vcpkg
- Direct3D 11-capable graphics hardware

The project uses a vcpkg manifest (`vcpkg.json`) to install FFmpeg and its x264 feature.

## Project Structure

```text
FFmpeg_UVTF/
|
|-- app/
|   |-- main.cpp
|
|-- common/
|   |-- FrameQueue.h
|   |-- IVideoInput.h
|   |-- IVideoOutput.h
|   |-- VideoFormat.h
|   |-- VideoFrame.h
|   |-- VideoIOStatus.h
|   |-- VideoStatistics.h
|
|-- input/
|   |-- FileInput.cpp
|   |-- FileInput.h
|   |-- RTPInput.cpp
|   |-- RTPInput.h
|   |-- SyntheticInput.cpp
|   |-- SyntheticInput.h
|
|-- output/
|   |-- RTPOutput.cpp
|   |-- RTPOutput.h
|
|-- rendering/
|   |-- D3D11Renderer.cpp
|   |-- D3D11Renderer.h
|
|-- CMakeLists.txt
|-- vcpkg.json
|-- .gitignore
|-- README.md
```

## Build Instructions

### 1. Clone the repository

```bat
git clone https://github.com/skandayogeesh-source2004/FFmpeg_UVTF.git
cd FFmpeg_UVTF
```

### 2. Configure vcpkg

Set `VCPKG_ROOT` to your vcpkg installation directory.

For the Visual Studio-managed vcpkg installation:

```bat
set "VCPKG_ROOT=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\vcpkg"
```

If you use a standalone vcpkg installation, replace this path with your own vcpkg directory.

### 3. Configure the project

```bat
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
```

CMake uses the manifest to install the required FFmpeg dependencies into the build directory.

### 4. Build the project

```bat
cmake --build build --config Release
```

The executable is generated at:

```text
build\Release\uvtf.exe
```

### 5. Configure runtime DLL access

In the same Command Prompt:

```bat
set "PATH=%CD%\build\vcpkg_installed\x64-windows\bin;%PATH%"
```

This makes the FFmpeg runtime DLLs available when launching UVTF.

## Usage

### Sender

```bat
build\Release\uvtf.exe --send "path\to\input.mp4" 127.0.0.1 50000
```

The sender reads the input video, decodes it, encodes it as H.264, packetizes the encoded stream into RTP, and transmits it over UDP.

It also generates an SDP description file named:

```text
uvtf_sender.sdp
```

### Receiver

Open a second Command Prompt in the project directory, configure the runtime DLL path, and run:

```bat
set "PATH=%CD%\build\vcpkg_installed\x64-windows\bin;%PATH%"
build\Release\uvtf.exe --receive uvtf_sender.sdp
```

The receiver reads the SDP configuration, receives the RTP stream, decodes H.264 frames, converts them to BGRA32, and renders them using Direct3D 11.

For a loopback test, start the receiver promptly after starting the sender.

### Command-line help

```bat
build\Release\uvtf.exe
```

## Validation

The sender and receiver have been tested using a same-machine loopback configuration.

Test input:

- File: `sample_1024x760_30fps.mp4`
- Resolution: 1024 × 760
- Frame rate: 30 FPS

### Loopback test results

| Metric | Frames |
|---|---:|
| Input frames decoded | 1085 |
| Frames submitted to encoder | 1085 |
| Encoded packets written | 1085 |
| Receiver packets demuxed | 1085 |
| H.264 frames decoded | 1085 |
| Frames rendered using Direct3D 11 | 1085 |

Additional results:

- Decoder errors: 0
- Renderer errors: 0
- Sender and receiver frame counts matched in the test.

These results represent a successful same-machine loopback test. They do not establish two-computer Ethernet performance.

## Current Limitations and Future Work

- Two-PC Ethernet validation has not yet been performed.
- UDP transport does not guarantee packet delivery.
- Receiver idle detection uses a timeout heuristic.
- Receiver responsiveness at the end of a stream is still under investigation.
- Dynamic resolution changes and aspect-ratio scaling are not yet implemented.

Future work can include:

- Two-PC Ethernet testing
- Improved receiver shutdown and responsiveness
- Dynamic resolution and aspect-ratio handling
- Network performance and packet-loss measurements
- Further development toward hardware-based video transport and ARINC 818 integration

## License

No license has been added yet. Until a license is selected and included, reuse and redistribution permissions are not explicitly granted.

## Author

**Skanda Yogeesh**

GitHub: https://github.com/skandayogeesh-source2004