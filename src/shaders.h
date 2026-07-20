// HLSL sources, compiled at runtime with D3DCompile.
//
// The sim shader is a port of the WebGL2 SIM_FS from
// https://evolvecode.io/alife/lenia.html (Emergent Garden / Artificial Life)
// with one structural change for power efficiency: kernel weights (bell-curve
// rings) are constant per species, so they are precomputed on the CPU into a
// structured buffer of taps (offset + per-lane weights, pre-masked by source
// channel and pre-normalized by the per-lane weight total). The per-pixel
// inner loop is pure Load + multiply-add. The growth curve stays per-pixel.
#pragma once

inline const char* kVertexShader = R"(
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    // Fullscreen triangle.
    float2 p = float2(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0);
    return float4(p, 0.0, 1.0);
}
)";

inline const char* kSimShader = R"(
Texture2D<float4> State : register(t0);

struct Tap {
    int2 d;
    float4 wR[4];   // lane weight when the lane reads channel R (else 0)
    float4 wG[4];
    float4 wB[4];
};
StructuredBuffer<Tap> Taps : register(t1);

cbuffer SimParams : register(b0) {
    int2 grid;
    int tapCount;
    float invT;
    float4 mu[4];
    float4 sigma[4];
    float4 eta[4];
    float4 dstR[4];  // lane -> destination channel masks
    float4 dstG[4];
    float4 dstB[4];
};

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    int2 ip = int2(pos.xy);
    float3 self = State.Load(int3(ip, 0)).rgb;

    // Weighted neighbourhood average per kernel lane (weights pre-normalized).
    float4 avg[4] = { 0.0.xxxx, 0.0.xxxx, 0.0.xxxx, 0.0.xxxx };
    for (int t = 0; t < tapCount; t++) {
        Tap tap = Taps[t];
        int2 q = ip + tap.d;
        q = (q % grid + grid) % grid;   // toroidal wrap
        float3 v = State.Load(int3(q, 0)).rgb;
        [unroll] for (int g = 0; g < 4; g++)
            avg[g] += tap.wR[g] * v.r + tap.wG[g] * v.g + tap.wB[g] * v.b;
    }

    // Growth: eta * (bell(avg, mu, sigma) * 2 - 1) / T, routed to dst channels.
    float3 growth = 0.0.xxx;
    [unroll] for (int g = 0; g < 4; g++) {
        float4 d = (avg[g] - mu[g]) / sigma[g];
        float4 gk = eta[g] * (exp(d * d * -0.5) * 2.0 - 1.0) * invT;
        growth.r += dot(gk, dstR[g]);
        growth.g += dot(gk, dstG[g]);
        growth.b += dot(gk, dstB[g]);
    }
    return float4(saturate(self + growth), 1.0);
}
)";

inline const char* kVisShader = R"(
Texture2D<float4> State : register(t0);

cbuffer VisParams : register(b0) {
    int2 grid;
    float2 res;
    float2 offset;  // buffer-pixel offset of grid cell (0,0)
    float px;       // buffer pixels per cell
    int mono;       // 1 = single-channel save: broadcast R to white
};

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    float2 cellf = (pos.xy - offset) / px;
    int2 c = int2(floor(cellf));
    float3 v;
    if (c.x < 0 || c.y < 0 || c.x >= grid.x || c.y >= grid.y)
        v = float3(0.09, 0.10, 0.13);           // flat dark navy outside the grid
    else {
        v = State.Load(int3(c, 0)).rgb;         // integer Load = NEAREST, crisp cells
        if (mono == 1) v = v.rrr;
    }

    // Strong circular vignette — microscope eyepiece falloff.
    float2 uv = (pos.xy / res) * 2.0 - 1.0;
    uv.x *= res.x / max(res.y, 1.0);            // aspect-correct circle
    float dist = length(uv);
    // Bright through ~0.62 radius (~35% larger aperture), then steep black-out toward the rim.
    float vignette = saturate(1.0 - smoothstep(0.62, 1.28, dist));
    vignette = pow(vignette, 1.6);
    v *= vignette;

    return float4(v, 1.0);
}
)";

// Premultiplied-alpha blit of a baked name strip (no D2D on the hot path).
// Viewport is set to the bottom strip; UVs cover that viewport 0..1.
inline const char* kOverlayVertexShader = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};
VSOut VSMain(uint id : SV_VertexID) {
    float2 p = float2(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0);
    VSOut o;
    o.pos = float4(p, 0.0, 1.0);
    o.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    return o;
}
)";

inline const char* kOverlayShader = R"(
Texture2D Overlay : register(t0);
SamplerState Samp : register(s0);

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return Overlay.Sample(Samp, uv);
}
)";
