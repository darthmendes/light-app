#pragma once

#include "ambient_config.hpp"
#include "ambient_types.hpp"

#include <functional>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

class ScreenCapture {
public:
    explicit ScreenCapture(const AmbientConfig& config);
    void run(const std::function<void(const AmbientColors&)>& onFrame);

private:
    bool initialize();
    void release();

    AmbientConfig config_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    int width_ = 0;
    int height_ = 0;
};