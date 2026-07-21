#pragma once
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite_3.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <random>
#include <string>
#include <vector>

#include "species.h"

struct ProbeResult {
    float mean = 0.0f;       // mean of all channel values
    float maxValue = 0.0f;   // hottest single channel value
    float meanDelta = 0.0f;  // mean |change| vs the previous probe
};

class Renderer {
public:
    bool Init(HWND hwnd, int width, int height, int cellScale, std::string* err);
    void Resize(int width, int height);  // caller should reload a species afterwards

    bool LoadSpecies(const Species& s, std::mt19937& rng, bool randomSoup = false);
    void ReseedCurrent(bool soup, std::mt19937& rng);
    bool HasSpecies() const { return hasSpecies_; }
    const Species& Current() const { return species_; }

    void StepAndPresent();

    // Death probe, deferred one cycle so Map never stalls the GPU:
    // call StartProbe(), then >= one frame later FetchProbe().
    void StartProbe();
    bool FetchProbe(ProbeResult* out);
    // Drop an in-flight probe (e.g. a pause interrupted the interval; comparing
    // across it would false-flag the grid as frozen).
    void CancelProbe() { probePending_ = false; }

    int GridW() const { return gridW_; }
    int GridH() const { return gridH_; }
    int TapCount() const { return tapCount_; }

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    void UploadState(const std::vector<float>& rgbaFloats);
    void DrawVis();
    void BakeNameOverlay();   // D2D once when the name / size changes
    void DrawNameOverlay();   // cheap D3D blit of the baked strip

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11RenderTargetView> backbufferRtv_;
    ComPtr<ID3D11VertexShader> vs_, overlayVs_;
    ComPtr<ID3D11PixelShader> simPs_, visPs_, overlayPs_;
    ComPtr<ID3D11Buffer> simCb_, visCb_, tapBuffer_;
    ComPtr<ID3D11ShaderResourceView> tapSrv_;
    ComPtr<ID3D11Texture2D> stateTex_[2], stagingTex_;
    ComPtr<ID3D11ShaderResourceView> stateSrv_[2];
    ComPtr<ID3D11RenderTargetView> stateRtv_[2];

    // Baked name strip (rebuilt on species load / resize); composited with blend.
    ComPtr<ID3D11Texture2D> overlayTex_;
    ComPtr<ID3D11ShaderResourceView> overlaySrv_;
    ComPtr<ID3D11BlendState> overlayBlend_;
    ComPtr<ID3D11SamplerState> overlaySamp_;
    int overlayH_ = 0;
    bool overlayReady_ = false;

    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> d2dContext_;
    ComPtr<IDWriteFactory> dwriteFactory_;
    ComPtr<IDWriteFontCollection> nameFontCollection_;
    ComPtr<IDWriteTextFormat> nameFormat_;
    ComPtr<ID2D1SolidColorBrush> nameFill_;
    ComPtr<ID2D1SolidColorBrush> nameShadow_;
    ComPtr<ID2D1SolidColorBrush> nameBackdrop_;
    bool overlayOk_ = false;

    Species species_;
    bool hasSpecies_ = false;
    int width_ = 0, height_ = 0, cellScale_ = 4;
    int gridW_ = 0, gridH_ = 0;
    int flip_ = 0;
    int tapCount_ = 0;
    bool probePending_ = false;
    std::vector<float> prevProbe_;  // per-texel RGB from the last mapped probe
};
