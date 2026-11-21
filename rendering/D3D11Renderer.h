#pragma once

#include "VideoFrame.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <string>

namespace uvtf
{

class D3D11Renderer
{
public:
    D3D11Renderer() = default;
    ~D3D11Renderer();

    D3D11Renderer(const D3D11Renderer&) = delete;
    D3D11Renderer& operator=(const D3D11Renderer&) = delete;

    bool open(const VideoFormat& format, const std::wstring& title = L"UVTF - RTP Receiver");
    bool render(const VideoFrame& frame);
    bool pumpMessages();
    bool isCloseRequested() const noexcept;
    void close() noexcept;
    bool isOpen() const noexcept;

    const std::string& getLastError() const noexcept;

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;

    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11Texture2D* backBuffer_ = nullptr;

    VideoFormat format_;
    bool running_ = false;
    bool closeRequested_ = false;
    std::string lastError_;
};

}
