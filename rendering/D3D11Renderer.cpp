#include "D3D11Renderer.h"

#include <cstdint>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace uvtf
{

namespace
{
constexpr wchar_t WINDOW_CLASS_NAME[] = L"UVTF_D3D11_RECEIVER_WINDOW";
}

D3D11Renderer::~D3D11Renderer()
{
    close();
}

bool D3D11Renderer::open(
    const VideoFormat& format,
    const std::wstring& title)
{
    close();
    lastError_.clear();

    if (!format.isValid() ||
        format.pixelFormat != PixelFormat::BGRA32)
    {
        lastError_ = "D3D11 renderer requires a valid BGRA32 video format.";
        return false;
    }

    format_ = format;
    instance_ = GetModuleHandleW(nullptr);
    closeRequested_ = false;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &D3D11Renderer::windowProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS_NAME;

    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        lastError_ = "Could not register the Direct3D receiver window class.";
        return false;
    }

    RECT rect{0, 0, format.width, format.height};
    const DWORD style =
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&rect, style, FALSE);

    hwnd_ = CreateWindowExW(
        0,
        WINDOW_CLASS_NAME,
        title.c_str(),
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        instance_,
        this
    );

    if (!hwnd_)
    {
        lastError_ = "Could not create the Direct3D receiver window.";
        close();
        return false;
    }

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferDesc.Width = static_cast<UINT>(format.width);
    swapDesc.BufferDesc.Height = static_cast<UINT>(format.height);
    swapDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapDesc.BufferDesc.RefreshRate.Numerator = 0;
    swapDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = 2;
    swapDesc.OutputWindow = hwnd_;
    swapDesc.Windowed = TRUE;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel{};
    const D3D_FEATURE_LEVEL requestedLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1
    };

    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        requestedLevels,
        ARRAYSIZE(requestedLevels),
        D3D11_SDK_VERSION,
        &swapDesc,
        &swapChain_,
        &device_,
        &featureLevel,
        &context_
    );

    if (FAILED(hr))
    {
        lastError_ = "D3D11CreateDeviceAndSwapChain failed (HRESULT " +
            std::to_string(static_cast<unsigned long>(hr)) + ").";
        close();
        return false;
    }

    const HRESULT bufferResult = swapChain_->GetBuffer(
        0,
        __uuidof(ID3D11Texture2D),
        reinterpret_cast<void**>(&backBuffer_)
    );

    if (FAILED(bufferResult))
    {
        lastError_ = "Could not obtain the Direct3D swap-chain back buffer.";
        close();
        return false;
    }

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    running_ = true;
    return true;
}

bool D3D11Renderer::render(const VideoFrame& frame)
{
    if (!isOpen())
    {
        lastError_ = "Direct3D renderer is not open.";
        return false;
    }

    if (!frame.isValid() ||
        frame.format.pixelFormat != PixelFormat::BGRA32 ||
        frame.format.width != format_.width ||
        frame.format.height != format_.height ||
        frame.planeCount != 1)
    {
        lastError_ = "VideoFrame does not match the D3D11 renderer format.";
        return false;
    }

    const VideoPlane& plane = frame.planes[0];

    context_->UpdateSubresource(
        backBuffer_,
        0,
        nullptr,
        frame.data.data() + plane.offset,
        static_cast<UINT>(plane.stride),
        0
    );

    const HRESULT result = swapChain_->Present(1, 0);
    if (FAILED(result))
    {
        lastError_ = "Direct3D Present failed (HRESULT " +
            std::to_string(static_cast<unsigned long>(result)) + ").";
        return false;
    }

    return true;
}

bool D3D11Renderer::pumpMessages()
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        if (message.message == WM_QUIT)
        {
            running_ = false;
            closeRequested_ = true;
            return false;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return running_ && !closeRequested_;
}

bool D3D11Renderer::isCloseRequested() const noexcept
{
    return closeRequested_;
}

void D3D11Renderer::close() noexcept
{
    running_ = false;

    if (context_)
        context_->ClearState();

    if (backBuffer_)
    {
        backBuffer_->Release();
        backBuffer_ = nullptr;
    }

    if (swapChain_)
    {
        swapChain_->Release();
        swapChain_ = nullptr;
    }

    if (context_)
    {
        context_->Release();
        context_ = nullptr;
    }

    if (device_)
    {
        device_->Release();
        device_ = nullptr;
    }

    if (hwnd_)
    {
        const HWND window = hwnd_;
        hwnd_ = nullptr;
        DestroyWindow(window);
    }
}

bool D3D11Renderer::isOpen() const noexcept
{
    return hwnd_ && swapChain_ && device_ && context_ &&
        backBuffer_ && running_ && !closeRequested_;
}

const std::string& D3D11Renderer::getLastError() const noexcept
{
    return lastError_;
}

LRESULT CALLBACK D3D11Renderer::windowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    D3D11Renderer* self = reinterpret_cast<D3D11Renderer*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA)
    );

    if (message == WM_NCCREATE)
    {
        const auto* create =
            reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<D3D11Renderer*>(create->lpCreateParams);
        SetWindowLongPtrW(
            hwnd,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self)
        );
    }

    if (self)
        return self->handleMessage(hwnd, message, wParam, lParam);

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT D3D11Renderer::handleMessage(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    if (message == WM_CLOSE)
    {
        // Do not destroy the HWND here. Notify the application so it can
        // request decoder drain and release D3D resources in the right order.
        closeRequested_ = true;
        running_ = false;
        return 0;
    }

    if (message == WM_DESTROY)
    {
        running_ = false;
        hwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}
