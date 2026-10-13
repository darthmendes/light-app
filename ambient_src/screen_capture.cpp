#include "screen_capture.hpp"

#include "color_processor.hpp"

#include <chrono>
#include <iostream>
#include <thread>

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

ScreenCapture::ScreenCapture(const AmbientConfig& config) : config_(config) {}

void ScreenCapture::release() {
    staging_.Reset();
    duplication_.Reset();
    context_.Reset();
    device_.Reset();
    width_ = 0;
    height_ = 0;
}

bool ScreenCapture::initialize() {
    release();
    D3D_FEATURE_LEVEL featureLevel{};
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device_, &featureLevel, &context_);
    if (FAILED(result)) {
        std::cerr << "D3D11CreateDevice failed: 0x" << std::hex << result << std::dec << '\n';
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(device_.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->EnumOutputs(0, &output)) || FAILED(output.As(&output1))) {
        std::cerr << "Could not find a Desktop Duplication output.\n";
        release();
        return false;
    }

    DXGI_OUTPUT_DESC outputDesc{};
    output->GetDesc(&outputDesc);
    width_ = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
    height_ = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
    result = output1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(result)) {
        std::cerr << "DuplicateOutput failed: 0x" << std::hex << result << std::dec << '\n';
        release();
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc{};
    stagingDesc.Width = static_cast<UINT>(width_);
    stagingDesc.Height = static_cast<UINT>(height_);
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    result = device_->CreateTexture2D(&stagingDesc, nullptr, &staging_);
    if (FAILED(result)) {
        std::cerr << "CreateTexture2D staging failed: 0x" << std::hex << result << std::dec << '\n';
        release();
        return false;
    }

    std::cout << "Desktop Duplication active: " << width_ << 'x' << height_ << '\n';
    return true;
}

void ScreenCapture::run(const std::function<void(const AmbientColors&)>& onFrame) {
    ColorProcessor processor(config_);
    const auto frameInterval = std::chrono::duration<double>(1.0 / config_.fps);
    auto deadline = Clock::now();

    while (true) {
        deadline += std::chrono::duration_cast<Clock::duration>(frameInterval);
        if (!duplication_ && !initialize()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }

        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        ComPtr<IDXGIResource> resource;
        const HRESULT result = duplication_->AcquireNextFrame(16, &frameInfo, &resource);
        if (result == DXGI_ERROR_WAIT_TIMEOUT) {
            std::this_thread::sleep_until(deadline);
            continue;
        }
        if (result == DXGI_ERROR_ACCESS_LOST || FAILED(result)) {
            if (result != DXGI_ERROR_ACCESS_LOST) {
                std::cerr << "AcquireNextFrame failed: 0x" << std::hex << result << std::dec << '\n';
            }
            release();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }

        ComPtr<ID3D11Texture2D> frame;
        const HRESULT textureResult = resource.As(&frame);
        if (SUCCEEDED(textureResult)) {
            context_->CopyResource(staging_.Get(), frame.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                onFrame(processor.process(static_cast<const std::uint8_t*>(mapped.pData),
                    width_, height_, static_cast<int>(mapped.RowPitch)));
                context_->Unmap(staging_.Get(), 0);
            }
        }
        duplication_->ReleaseFrame();

        const auto now = Clock::now();
        if (now < deadline) std::this_thread::sleep_until(deadline);
        else deadline = now;
    }
}