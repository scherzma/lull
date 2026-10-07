// Fullscreen triangle + final blit (upscale, dither, fade, premultiply).

cbuffer Params : register(b0)
{
    float2 res;
    float  time;
    float  breath;
    float  opacity;
    float  frame;
    float  seed;
    float  pad;
    float2 origin;   // where the scene's centre sits, in pixels (0 = canvas centre)
    float  normH;    // pixels per scene unit (0 = canvas height)
    float  pad2;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}

Texture2D    src  : register(t0);
SamplerState samp : register(s0);

float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * .1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float4 PSBlit(VSOut i) : SV_Target
{
    float3 c = src.SampleLevel(samp, i.uv, 0).rgb;
    // Triangular dither so soft gradients never band.
    float2 p = i.pos.xy + frac(frame * 0.6180339) * 113.0;
    float n = hash12(p) + hash12(p + 71.37) - 1.0;
    c = saturate(c + n / 255.0);
    return float4(c * opacity, opacity);
}
