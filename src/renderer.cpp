#include "renderer.h"

#include <d2d1helper.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>

#include "shaders.h"
#include "util.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace {

// IEEE 754 float <-> half conversions (state textures are RGBA16F).
uint16_t FloatToHalf(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t exp = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFF;
    if (exp <= 0) return (uint16_t)sign;                       // flush denorms/underflow to 0
    if (exp >= 31) return (uint16_t)(sign | 0x7C00);           // overflow -> inf
    return (uint16_t)(sign | (exp << 10) | (mant >> 13));
}

float HalfToFloat(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t x;
    if (exp == 0) {
        if (mant == 0) x = sign;
        else {  // subnormal half
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) { mant <<= 1; exp--; }
            mant &= 0x3FF;
            x = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        x = sign | 0x7F800000 | (mant << 13);
    } else {
        x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &x, 4);
    return f;
}

bool CompileShader(const char* src, const char* entry, const char* target,
                   ID3DBlob** blob, std::string* err) {
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &errors);
    if (FAILED(hr)) {
        if (err) {
            *err = "shader compile failed";
            if (errors) *err += std::string(": ") + (const char*)errors->GetBufferPointer();
        }
        return false;
    }
    return true;
}

struct VisConstants {
    int32_t gridW, gridH;
    float resX, resY;
    float offX, offY;
    float px;
    int32_t mono;
};
static_assert(sizeof(VisConstants) == 32);

std::filesystem::path FindOverlayFont() {
    const std::filesystem::path candidates[] = {
        ExeDir() / "Digital7Italic.ttf",
        ExeDir().parent_path() / "fonts" / "Digital7Italic.ttf",
    };
    for (const auto& p : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p;
    }
    return {};
}

// Load Digital-7 as a private DirectWrite collection; returns family name used.
bool LoadPrivateFont(IDWriteFactory* factory, const std::filesystem::path& path,
                     IDWriteFontCollection** collectionOut, std::wstring* familyOut) {
    Microsoft::WRL::ComPtr<IDWriteFactory5> factory5;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory5)))) return false;

    Microsoft::WRL::ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(factory5->CreateFontSetBuilder(&builder))) return false;

    Microsoft::WRL::ComPtr<IDWriteFontFile> fontFile;
    if (FAILED(factory5->CreateFontFileReference(path.c_str(), nullptr, &fontFile))) return false;
    if (FAILED(builder->AddFontFile(fontFile.Get()))) return false;

    Microsoft::WRL::ComPtr<IDWriteFontSet> fontSet;
    if (FAILED(builder->CreateFontSet(&fontSet))) return false;

    Microsoft::WRL::ComPtr<IDWriteFontCollection1> collection1;
    if (FAILED(factory5->CreateFontCollectionFromFontSet(fontSet.Get(), &collection1))) return false;

    if (collection1->GetFontFamilyCount() < 1) return false;
    Microsoft::WRL::ComPtr<IDWriteFontFamily> family;
    if (FAILED(collection1->GetFontFamily(0, &family))) return false;
    Microsoft::WRL::ComPtr<IDWriteLocalizedStrings> names;
    if (FAILED(family->GetFamilyNames(&names))) return false;

    UINT32 index = 0;
    BOOL exists = FALSE;
    names->FindLocaleName(L"en-us", &index, &exists);
    if (!exists) index = 0;
    UINT32 len = 0;
    names->GetStringLength(index, &len);
    familyOut->assign(len, L'\0');
    names->GetString(index, familyOut->data(), len + 1);

    *collectionOut = collection1.Detach();
    return true;
}

}  // namespace

bool Renderer::Init(HWND hwnd, int width, int height, int cellScale, std::string* err) {
    width_ = width;
    height_ = height;
    cellScale_ = cellScale;

    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        if (err) *err = "CreateDXGIFactory1 failed";
        return false;
    }

    // Prefer the low-power adapter (iGPU) so hybrid laptops never wake the dGPU.
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory.As(&factory6)))
        factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_MINIMUM_POWER,
                                             IID_PPV_ARGS(&adapter));

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    // BGRA_SUPPORT is required for Direct2D to draw on the DXGI backbuffer.
    UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(adapter.Get(),
                                   adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, deviceFlags, levels, 1, D3D11_SDK_VERSION,
                                   &device_, nullptr, &ctx_);
    if (FAILED(hr)) {
        if (err) *err = "D3D11CreateDevice failed";
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width;
    sd.Height = height;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &sd, nullptr, nullptr, &swap_))) {
        if (err) *err = "CreateSwapChainForHwnd failed";
        return false;
    }
    ComPtr<ID3D11Texture2D> backbuffer;
    swap_->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &backbufferRtv_);

    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!CompileShader(kVertexShader, "VSMain", "vs_5_0", &vsBlob, err)) return false;
    device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_);
    if (!CompileShader(kSimShader, "PSMain", "ps_5_0", &psBlob, err)) return false;
    device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &simPs_);
    psBlob.Reset();
    if (!CompileShader(kVisShader, "PSMain", "ps_5_0", &psBlob, err)) return false;
    device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &visPs_);
    psBlob.Reset();
    if (!CompileShader(kOverlayShader, "PSMain", "ps_5_0", &psBlob, err)) return false;
    device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr,
                               &overlayPs_);
    ComPtr<ID3DBlob> overlayVsBlob;
    if (!CompileShader(kOverlayVertexShader, "VSMain", "vs_5_0", &overlayVsBlob, err)) return false;
    device_->CreateVertexShader(overlayVsBlob->GetBufferPointer(), overlayVsBlob->GetBufferSize(),
                                nullptr, &overlayVs_);

    // The fullscreen triangle is CCW (GL convention); default D3D11 culling
    // would drop it, so disable culling outright.
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    ComPtr<ID3D11RasterizerState> raster;
    if (SUCCEEDED(device_->CreateRasterizerState(&rd, &raster)))
        ctx_->RSSetState(raster.Get());

    // Premultiplied alpha: D2D writes PMA into the baked strip.
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&blend, &overlayBlend_))) return false;

    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device_->CreateSamplerState(&samp, &overlaySamp_))) return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(SimConstants);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device_->CreateBuffer(&cbd, nullptr, &simCb_))) return false;
    cbd.ByteWidth = sizeof(VisConstants);
    if (FAILED(device_->CreateBuffer(&cbd, nullptr, &visCb_))) return false;

    // Sim grid: screen / cellScale (site: CELL = 4, min 64), fixed for a monitor size.
    gridW_ = std::max(64, (width + cellScale - 1) / cellScale);
    gridH_ = std::max(64, (height + cellScale - 1) / cellScale);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = gridW_;
    td.Height = gridH_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    for (int i = 0; i < 2; i++) {
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &stateTex_[i]))) return false;
        device_->CreateShaderResourceView(stateTex_[i].Get(), nullptr, &stateSrv_[i]);
        device_->CreateRenderTargetView(stateTex_[i].Get(), nullptr, &stateRtv_[i]);
    }
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &stagingTex_))) return false;

    // Optional species-name HUD (Direct2D bake + cheap D3D composite).
    overlayOk_ = false;
    ComPtr<ID2D1Factory1> factory1;
    ComPtr<IDXGIDevice> dxgiDevice;
    bool dwriteOk =
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&factory1))) &&
        SUCCEEDED(device_.As(&dxgiDevice)) &&
        SUCCEEDED(factory1->CreateDevice(dxgiDevice.Get(), &d2dDevice_)) &&
        SUCCEEDED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext_)) &&
        SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                      reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf())));

    std::wstring fontFamily = L"Segoe UI";
    if (dwriteOk) {
        auto fontPath = FindOverlayFont();
        std::wstring privateFamily;
        if (!fontPath.empty() &&
            LoadPrivateFont(dwriteFactory_.Get(), fontPath, &nameFontCollection_, &privateFamily)) {
            fontFamily = privateFamily;
            LogLine("renderer: overlay font '%s' from %s", WideToUtf8(fontFamily).c_str(),
                    fontPath.string().c_str());
        } else {
            LogLine("renderer: Digital7Italic.ttf not found; falling back to Segoe UI");
        }
    }

    if (dwriteOk &&
        SUCCEEDED(dwriteFactory_->CreateTextFormat(
            fontFamily.c_str(), nameFontCollection_.Get(), DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 36.0f, L"en-us", &nameFormat_)) &&
        SUCCEEDED(d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.78f), &nameBackdrop_)) &&
        SUCCEEDED(d2dContext_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.70f), &nameShadow_)) &&
        SUCCEEDED(d2dContext_->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.95f), &nameFill_))) {
        d2dFactory_ = factory1;
        nameFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        nameFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        overlayOk_ = true;
        LogLine("renderer: name overlay ready");
    } else {
        LogLine("renderer: name overlay unavailable");
        d2dFactory_.Reset();
        d2dDevice_.Reset();
        d2dContext_.Reset();
        dwriteFactory_.Reset();
        nameFontCollection_.Reset();
        nameFormat_.Reset();
        nameFill_.Reset();
        nameShadow_.Reset();
        nameBackdrop_.Reset();
    }

    LogLine("renderer: %dx%d window, %dx%d grid", width, height, gridW_, gridH_);
    return true;
}

void Renderer::Resize(int width, int height) {
    if (width == width_ && height == height_) return;
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    backbufferRtv_.Reset();
    swap_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    ComPtr<ID3D11Texture2D> backbuffer;
    swap_->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    device_->CreateRenderTargetView(backbuffer.Get(), nullptr, &backbufferRtv_);
    width_ = width;
    height_ = height;

    int gw = std::max(64, (width + cellScale_ - 1) / cellScale_);
    int gh = std::max(64, (height + cellScale_ - 1) / cellScale_);
    if (gw != gridW_ || gh != gridH_) {
        gridW_ = gw;
        gridH_ = gh;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = gridW_;
        td.Height = gridH_;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        for (int i = 0; i < 2; i++) {
            stateTex_[i].Reset();
            stateSrv_[i].Reset();
            stateRtv_[i].Reset();
            device_->CreateTexture2D(&td, nullptr, &stateTex_[i]);
            device_->CreateShaderResourceView(stateTex_[i].Get(), nullptr, &stateSrv_[i]);
            device_->CreateRenderTargetView(stateTex_[i].Get(), nullptr, &stateRtv_[i]);
        }
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingTex_.Reset();
        device_->CreateTexture2D(&td, nullptr, &stagingTex_);
        hasSpecies_ = false;  // grid changed; caller must reload
    }
    probePending_ = false;
    prevProbe_.clear();
    overlayReady_ = false;
    if (hasSpecies_ && !species_.name.empty()) BakeNameOverlay();
}

void Renderer::UploadState(const std::vector<float>& rgbaFloats) {
    std::vector<uint16_t> halfs(rgbaFloats.size());
    for (size_t i = 0; i < rgbaFloats.size(); i++) halfs[i] = FloatToHalf(rgbaFloats[i]);
    ctx_->UpdateSubresource(stateTex_[0].Get(), 0, nullptr, halfs.data(),
                            gridW_ * 4 * sizeof(uint16_t), 0);
    // Mirror into the second buffer so a probe right after reseed sees the same data.
    ctx_->CopyResource(stateTex_[1].Get(), stateTex_[0].Get());
    flip_ = 0;
    probePending_ = false;
    prevProbe_.clear();
}

bool Renderer::LoadSpecies(const Species& s, std::mt19937& rng) {
    std::vector<Tap> taps = BuildTaps(s);
    if (taps.empty()) return false;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (UINT)(taps.size() * sizeof(Tap));
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(Tap);
    D3D11_SUBRESOURCE_DATA init{taps.data(), 0, 0};
    ComPtr<ID3D11Buffer> buf;
    if (FAILED(device_->CreateBuffer(&bd, &init, &buf))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_UNKNOWN;
    sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sv.Buffer.NumElements = (UINT)taps.size();
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device_->CreateShaderResourceView(buf.Get(), &sv, &srv))) return false;
    tapBuffer_ = buf;
    tapSrv_ = srv;

    SimConstants c = BuildConstants(s, gridW_, gridH_, (int)taps.size());
    ctx_->UpdateSubresource(simCb_.Get(), 0, nullptr, &c, 0, 0);

    species_ = s;
    hasSpecies_ = true;
    tapCount_ = (int)taps.size();
    UploadState(BuildInitGrid(s, gridW_, gridH_, rng));
    BakeNameOverlay();
    LogLine("species: %s (R=%.1f T=%.1f kernels=%d taps=%zu %s)", s.name.c_str(), s.R, s.T,
            s.numKernels, taps.size(), s.init.isSeed ? "seed" : "soup");
    return true;
}

void Renderer::ReseedCurrent(bool soup, std::mt19937& rng) {
    if (!hasSpecies_) return;
    UploadState(soup ? BuildRandomSoup(species_, gridW_, gridH_, rng)
                     : BuildInitGrid(species_, gridW_, gridH_, rng));
}

void Renderer::StepAndPresent() {
    if (!hasSpecies_) return;

    // Sim pass: state[flip] -> state[1-flip].
    int src = flip_, dst = 1 - flip_;
    ID3D11ShaderResourceView* nullSrvs[2] = {nullptr, nullptr};
    ctx_->PSSetShaderResources(0, 2, nullSrvs);
    ctx_->OMSetRenderTargets(1, stateRtv_[dst].GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp{0, 0, (float)gridW_, (float)gridH_, 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(simPs_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = {stateSrv_[src].Get(), tapSrv_.Get()};
    ctx_->PSSetShaderResources(0, 2, srvs);
    ctx_->PSSetConstantBuffers(0, 1, simCb_.GetAddressOf());
    ctx_->Draw(3, 0);
    flip_ = dst;

    DrawVis();
    DrawNameOverlay();
    swap_->Present(0, 0);
}

void Renderer::BakeNameOverlay() {
    overlayReady_ = false;
    overlayTex_.Reset();
    overlaySrv_.Reset();
    if (!overlayOk_ || !d2dContext_ || species_.name.empty() || width_ <= 0 || height_ <= 0) return;

    // Bottom strip only — keeps the bake cheap and the blit tiny.
    constexpr int kPadBottom = 72;
    constexpr int kLineH = 62;
    overlayH_ = kPadBottom + kLineH + 8;
    if (overlayH_ > height_) overlayH_ = height_;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)width_;
    td.Height = (UINT)overlayH_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &overlayTex_))) return;
    if (FAILED(device_->CreateShaderResourceView(overlayTex_.Get(), nullptr, &overlaySrv_))) return;

    ComPtr<IDXGISurface> surface;
    if (FAILED(overlayTex_.As(&surface))) return;

    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1Bitmap1> target;
    if (FAILED(d2dContext_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &target))) return;

    std::wstring name = Utf8ToWide(species_.name);
    const float lineH = (float)kLineH;
    const float boxTop = (float)overlayH_ - (float)kPadBottom - lineH;
    const float boxBottom = (float)overlayH_ - (float)kPadBottom;
    D2D1_RECT_F box = D2D1::RectF(0.0f, boxTop, (float)width_, boxBottom);

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwriteFactory_->CreateTextLayout(name.c_str(), (UINT32)name.size(), nameFormat_.Get(),
                                                (float)width_, lineH, &layout)))
        return;
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);

    const float padH = 18.0f;
    const float padV = 10.0f;
    const float cx = (float)width_ * 0.5f;
    const float cy = (boxTop + boxBottom) * 0.5f;
    D2D1_ROUNDED_RECT backdrop{};
    backdrop.rect = D2D1::RectF(cx - metrics.width * 0.5f - padH, cy - metrics.height * 0.5f - padV,
                                cx + metrics.width * 0.5f + padH, cy + metrics.height * 0.5f + padV);
    backdrop.radiusX = 8.0f;
    backdrop.radiusY = 8.0f;

    // One Flush here is fine — bake runs only on species change / resize.
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    ctx_->Flush();

    d2dContext_->SetTarget(target.Get());
    d2dContext_->BeginDraw();
    d2dContext_->Clear(D2D1::ColorF(0, 0, 0, 0));
    d2dContext_->FillRoundedRectangle(backdrop, nameBackdrop_.Get());
    D2D1_RECT_F shadowBox = box;
    shadowBox.left += 1.5f;
    shadowBox.top += 1.5f;
    shadowBox.right += 1.5f;
    shadowBox.bottom += 1.5f;
    d2dContext_->DrawText(name.c_str(), (UINT32)name.size(), nameFormat_.Get(), shadowBox,
                          nameShadow_.Get());
    d2dContext_->DrawText(name.c_str(), (UINT32)name.size(), nameFormat_.Get(), box,
                          nameFill_.Get());
    d2dContext_->EndDraw();
    d2dContext_->SetTarget(nullptr);

    overlayReady_ = true;
}

void Renderer::DrawNameOverlay() {
    if (!overlayReady_ || !overlaySrv_) return;

    // Viewport maps the strip onto the bottom of the backbuffer.
    const float top = (float)(height_ - overlayH_);
    D3D11_VIEWPORT vp{0, top, (float)width_, (float)overlayH_, 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetRenderTargets(1, backbufferRtv_.GetAddressOf(), nullptr);
    ctx_->VSSetShader(overlayVs_.Get(), nullptr, 0);
    ctx_->PSSetShader(overlayPs_.Get(), nullptr, 0);
    ctx_->PSSetShaderResources(0, 1, overlaySrv_.GetAddressOf());
    ctx_->PSSetSamplers(0, 1, overlaySamp_.GetAddressOf());
    float blendFactor[4] = {0, 0, 0, 0};
    ctx_->OMSetBlendState(overlayBlend_.Get(), blendFactor, 0xffffffff);
    ctx_->Draw(3, 0);
    ctx_->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx_->PSSetShaderResources(0, 1, &nullSrv);
}

void Renderer::DrawVis() {
    // Uniform scale that fully covers the window (grid aspect ~= window aspect,
    // so at most half a cell is cropped per edge); centered.
    float px = std::max((float)width_ / gridW_, (float)height_ / gridH_);
    VisConstants v{};
    v.gridW = gridW_;
    v.gridH = gridH_;
    v.resX = (float)width_;
    v.resY = (float)height_;
    v.offX = (width_ - gridW_ * px) * 0.5f;
    v.offY = (height_ - gridH_ * px) * 0.5f;
    v.px = px;
    v.mono = species_.mono ? 1 : 0;
    ctx_->UpdateSubresource(visCb_.Get(), 0, nullptr, &v, 0, 0);

    ID3D11ShaderResourceView* nullSrvs[2] = {nullptr, nullptr};
    ctx_->PSSetShaderResources(0, 2, nullSrvs);
    ctx_->OMSetRenderTargets(1, backbufferRtv_.GetAddressOf(), nullptr);
    D3D11_VIEWPORT vp{0, 0, (float)width_, (float)height_, 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(visPs_.Get(), nullptr, 0);
    ctx_->PSSetShaderResources(0, 1, stateSrv_[flip_].GetAddressOf());
    ctx_->PSSetConstantBuffers(0, 1, visCb_.GetAddressOf());
    ctx_->Draw(3, 0);
}

void Renderer::StartProbe() {
    if (!hasSpecies_) return;
    ctx_->CopyResource(stagingTex_.Get(), stateTex_[flip_].Get());
    probePending_ = true;
}

bool Renderer::FetchProbe(ProbeResult* out) {
    if (!probePending_) return false;
    probePending_ = false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    // Copied a full probe-interval ago, so this Map does not stall.
    if (FAILED(ctx_->Map(stagingTex_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

    size_t count = (size_t)gridW_ * gridH_ * 3;
    bool havePrev = prevProbe_.size() == count;
    if (!havePrev) prevProbe_.resize(count);

    double sum = 0.0, deltaSum = 0.0;
    float maxv = 0.0f;
    size_t idx = 0;
    for (int y = 0; y < gridH_; y++) {
        const uint16_t* row = (const uint16_t*)((const uint8_t*)mapped.pData + (size_t)y * mapped.RowPitch);
        for (int x = 0; x < gridW_; x++) {
            for (int ch = 0; ch < 3; ch++) {
                float f = HalfToFloat(row[x * 4 + ch]);
                sum += f;
                maxv = std::max(maxv, f);
                if (havePrev) deltaSum += std::fabs(f - prevProbe_[idx]);
                prevProbe_[idx++] = f;
            }
        }
    }
    ctx_->Unmap(stagingTex_.Get(), 0);

    out->mean = (float)(sum / count);
    out->maxValue = maxv;
    out->meanDelta = havePrev ? (float)(deltaSum / count) : 1.0f;
    return true;
}
