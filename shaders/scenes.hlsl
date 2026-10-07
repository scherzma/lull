// Lull scenes. Compiled once per scene with /D SCENE=n.
// Coordinates: p.y in [-0.5, 0.5] (up), p.x scaled by aspect. Colors mixed in linear, output sRGB.

cbuffer Params : register(b0)
{
    float2 res;
    float  time;
    float  breath;   // 0 = exhaled, 1 = inhaled (Breathe scene)
    float  opacity;
    float  frame;
    float  gridSources;   // Grid tuning: wave sources (1..8)
    float  gridIntensity; //              0..1: fold density, push, flares
    float2 origin;   // where the scene's centre sits, in pixels (0 = canvas centre)
    float  normH;    // pixels per scene unit (0 = canvas height)
    float  gridSeed;      // Grid: random environment for this lull (0 = classic layout)
    float  gridBreak;     // Grid: height at which the waves break into a superwave (0 = never)
    float  gridSurge;     // Grid: 0 calm .. 1 while the highest wave on screen has broken
    float  gridDay;       // Grid: 0 = night look, 1 = day look
    float  pad3;
};

#ifndef SCENE
#define SCENE 0
#endif

static const float PI = 3.14159265;

float3 lin(float3 c) { return pow(c, 2.2); }
float3 hexs(uint h) { return float3((h >> 16) & 255, (h >> 8) & 255, h & 255) / 255.0; }
float3 hexc(uint h) { return lin(hexs(h)); }
float3 toSrgb(float3 c) { return pow(saturate(c), 1.0 / 2.2); }

float hash11(float p) { p = frac(p * .1031); p *= p + 33.33; p *= p + p; return frac(p); }
float hash12(float2 p) { float3 p3 = frac(float3(p.xyx) * .1031); p3 += dot(p3, p3.yzx + 33.33); return frac((p3.x + p3.y) * p3.z); }
float2 hash21(float p) { float3 p3 = frac(p * float3(.1031, .1030, .0973)); p3 += dot(p3, p3.yzx + 33.33); return frac((p3.xx + p3.yz) * p3.zy); }

// 2D simplex noise (Ashima Arts / Stefan Gustavson, MIT).
float3 mod289(float3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
float2 mod289(float2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
float3 permute(float3 x) { return mod289(((x * 34.0) + 1.0) * x); }
float snoise(float2 v)
{
    const float4 C = float4(0.211324865405187, 0.366025403784439, -0.577350269189626, 0.024390243902439);
    float2 i = floor(v + dot(v, C.yy));
    float2 x0 = v - i + dot(i, C.xx);
    float2 i1 = (x0.x > x0.y) ? float2(1.0, 0.0) : float2(0.0, 1.0);
    float4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;
    i = mod289(i);
    float3 p = permute(permute(i.y + float3(0.0, i1.y, 1.0)) + i.x + float3(0.0, i1.x, 1.0));
    float3 m = max(0.5 - float3(dot(x0, x0), dot(x12.xy, x12.xy), dot(x12.zw, x12.zw)), 0.0);
    m = m * m; m = m * m;
    float3 x = 2.0 * frac(p * C.www) - 1.0;
    float3 h = abs(x) - 0.5;
    float3 ox = floor(x + 0.5);
    float3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);
    float3 g;
    g.x = a0.x * x0.x + h.x * x0.y;
    g.yz = a0.yz * x12.xz + h.yz * x12.yw;
    return 130.0 * dot(m, g);
}

float fbm(float2 p)
{
    float s = 0.0, a = 0.5;
    const float2x2 r = float2x2(0.8, -0.6, 0.6, 0.8);
    [unroll] for (int i = 0; i < 4; i++) { s += a * snoise(p); p = mul(r, p) * 2.03; a *= 0.5; }
    return s;
}

// ---------------------------------------------------------------- Aurora
float3 sceneAurora(float2 p, float t, float aa, float aspect)
{
    float y = p.y + 0.5;
    float3 col = lerp(hexc(0x1C3150), hexc(0x101A36), smoothstep(0.0, 0.5, y));
    col = lerp(col, hexc(0x070B1C), smoothstep(0.5, 1.0, y));

    // Stars, sparse and slowly twinkling.
    float2 sp = p * 70.0;
    float2 id = floor(sp);
    float h = hash12(id + 13.0);
    float2 f = frac(sp) - 0.5 - (hash21(h * 97.0) - 0.5) * 0.6;
    float star = smoothstep(0.09, 0.0, length(f)) * step(0.93, h);
    star *= 0.55 + 0.45 * sin(t * 0.7 + h * 60.0);
    col += star * 0.35 * smoothstep(0.35, 0.8, y);

    // Curtains of light.
    float3 aur = 0;
    [unroll] for (int i = 0; i < 3; i++)
    {
        float fi = i;
        float x = p.x * 0.8 + fi * 1.9;
        float curve = 0.02 + fi * 0.07
                    + 0.10 * snoise(float2(x * 0.6 + t * 0.012, fi * 3.1 + t * 0.008))
                    + 0.04 * sin(x * 1.4 + t * 0.04 + fi * 2.0);
        float d = p.y - curve;
        float edge = smoothstep(-0.03, 0.012, d);
        float fade = exp(-max(d, 0.0) * (5.0 + fi * 2.0));
        float rays = 0.45 + 0.55 * saturate(0.5 + 0.7 * snoise(float2(x * 6.0 + t * 0.03, fi * 7.0 + t * 0.02)));
        // Brightness varies along the curtain so it never looks like a flat wall.
        float patch = smoothstep(-0.4, 0.7, snoise(float2(x * 0.45 - t * 0.01, fi * 5.3)));
        float3 c = lerp(hexc(0x5EF0C0), hexc(0x9C7CF4), saturate(d * 3.0 + fi * 0.2));
        aur += c * edge * fade * rays * (0.15 + 0.85 * patch) * (0.55 - fi * 0.12);
    }
    col += aur;

    // Two soft mountain ridges.
    float m1 = -0.25 + 0.05 * snoise(float2(p.x * 1.1, 3.0)) + 0.018 * snoise(float2(p.x * 4.0, 7.0));
    float m2 = -0.34 + 0.04 * snoise(float2(p.x * 1.5 + 5.0, 1.0)) + 0.012 * snoise(float2(p.x * 5.0, 2.0));
    col = lerp(col, hexc(0x0D1529) + aur * 0.05, smoothstep(aa, -aa, p.y - m1));
    col = lerp(col, hexc(0x060A16), smoothstep(aa, -aa, p.y - m2));
    return col;
}

// ---------------------------------------------------------------- Drift (mesh gradient)
float3 sceneDrift(float2 p, float t, float aa, float aspect)
{
    float tt = t * 0.05;
    float2 q = p + 0.10 * float2(snoise(p * 1.1 + tt), snoise(p * 1.1 - tt + 4.0));

    // Blend in gamma space: keeps pastel mixes clean instead of greying out.
    float3 cols[5] = { hexs(0xFAC4AE), hexs(0xC3B8F7), hexs(0xF5B5D0), hexs(0xAEE3D6), hexs(0xFCE3BC) };
    float3 acc = hexs(0xF6EEEA) * 0.02;
    float wsum = 0.02;
    [unroll] for (int i = 0; i < 5; i++)
    {
        float fi = i;
        float2 c = float2(sin(tt * (0.9 + fi * 0.23) + fi * 1.7) * aspect * 0.42,
                          cos(tt * (0.7 + fi * 0.19) + fi * 2.9) * 0.36);
        float d = length(q - c);
        float w = exp(-d * d * 7.0);
        acc += cols[i] * w;
        wsum += w;
    }
    float3 col = lin(acc / wsum);
    // Gentle light from the top.
    col *= 0.94 + 0.08 * smoothstep(-0.5, 0.5, p.y);
    return col;
}

// ---------------------------------------------------------------- Tide (layered waves at dusk)
float3 sceneTide(float2 p, float t, float aa, float aspect)
{
    float y = p.y + 0.5;
    float3 col = lerp(hexc(0xFBE2CF), hexc(0xF0C8CF), smoothstep(0.45, 0.75, y));
    col = lerp(col, hexc(0xC9BCE6), smoothstep(0.72, 1.05, y));

    // Low sun with a halo.
    float2 sc = float2(aspect * 0.18, 0.04);
    float sd = length(p - sc);
    col += hexc(0xFFE9D2) * 0.35 * exp(-sd * 5.0);
    col = lerp(col, hexc(0xFFF4E8), smoothstep(0.075 + aa, 0.075 - aa, sd));

    const int N = 6;
    [unroll] for (int i = 0; i < N; i++)
    {
        float fi = i / (N - 1.0);
        float base = 0.0 - fi * 0.36;
        float freq = 2.2 + fi * 1.2;
        float speed = 0.015 + fi * 0.03;
        float amp = 0.012 + fi * 0.022;
        float x = p.x + i * 3.7;
        float h = base + amp * (0.65 * sin(x * freq + t * speed * 6.0 + i)
                              + 0.35 * snoise(float2(x * freq * 0.35 + t * speed, i * 4.3)));
        float3 lc = lerp(hexc(0xEDBFC4), hexc(0x2F3E62), pow(fi, 0.85));
        lc = lerp(lc, col, 0.18 * (1.0 - fi)); // haze on far layers
        lc *= 1.0 - 0.10 * saturate((h - p.y) * 4.0);
        // A thin bright crest catching the light.
        float crest = smoothstep(0.006, 0.0, abs(p.y - h + 0.002)) * (0.10 + 0.05 * fi);
        float m = smoothstep(aa, -aa, p.y - h);
        col = lerp(col, lc, m);
        col += crest * hexc(0xFFE6D6) * m;
    }
    return col;
}

// ---------------------------------------------------------------- Breathe (petal orb)
float3 sceneBreathe(float2 p, float t, float aa, float aspect)
{
    float b = breath;
    float r = length(p);
    float3 col = lerp(hexc(0x1E4650), hexc(0x0D1F27), smoothstep(0.0, 0.85, r));

    // Six translucent petals that bloom outward on the in-breath.
    float rot = t * 0.02 + b * (PI / 3.0);
    float spread = lerp(0.045, 0.13, b);
    float pr = lerp(0.075, 0.13, b);
    float alpha = lerp(0.20, 0.30, b); // overlap stacks up when closed, so start dimmer
    [unroll] for (int i = 0; i < 6; i++)
    {
        float a = rot + i * (PI / 3.0);
        float2 c = spread * float2(cos(a), sin(a));
        float d = length(p - c);
        float m = smoothstep(pr + aa, pr - 0.006, d);
        float3 pc = lerp(hexc(0x9BE3D2), hexc(0x79B8D8), (i & 1) ? 1.0 : 0.2);
        col = 1.0 - (1.0 - col) * (1.0 - pc * m * alpha); // screen blend
    }
    col += hexc(0x7FD9C8) * 0.05 * exp(-r * r / (0.03 + 0.06 * b));

    // A few motes drifting through.
    [unroll] for (int k = 0; k < 12; k++)
    {
        float2 h2 = hash21(k * 7.31 + 1.0);
        float life = frac(h2.y + t * (0.006 + 0.006 * h2.x));
        float2 c = float2((h2.x - 0.5) * aspect, -0.55 + life * 1.1);
        c.x += 0.02 * sin(t * 0.2 + k);
        float d = length(p - c);
        col += hexc(0xBFF3E6) * 0.10 * smoothstep(0.004, 0.0, d) * sin(life * PI);
    }
    return col;
}

// ---------------------------------------------------------------- Lanterns (bokeh)
float3 sceneLanterns(float2 p, float t, float aa, float aspect)
{
    float y = p.y + 0.5;
    float3 col = lerp(hexc(0x4A2440), hexc(0x251A3E), smoothstep(0.0, 0.55, y));
    col = lerp(col, hexc(0x120E26), smoothstep(0.55, 1.0, y));
    col += hexc(0x9A4E55) * 0.35 * exp(-(p.x * p.x * 1.2 + (p.y + 0.62) * (p.y + 0.62) * 5.0));

    float3 pal[4] = { hexc(0xFFC38A), hexc(0xFFA08F), hexc(0xFFE2A8), hexc(0xF6B3D6) };
    [unroll] for (int L = 0; L < 3; L++)
    {
        float fl = L / 2.0;
        float size = lerp(0.010, 0.055, fl);
        float blur = lerp(0.25, 0.85, fl);
        float speed = lerp(0.006, 0.016, fl);
        float alpha = lerp(0.50, 0.20, fl);
        [loop] for (int i = 0; i < 14; i++)
        {
            float key = i * 13.7 + L * 101.3 + 3.0;
            float s = hash11(key);
            float2 h2 = hash21(key * 1.91);
            float life = frac(h2.y + t * speed * (0.7 + 0.6 * s));
            float2 c = float2((h2.x - 0.5) * aspect * 1.1 + 0.03 * sin(t * 0.15 * (0.5 + s) + s * 30.0),
                              -0.62 + life * 1.24);
            float rr = size * (0.6 + 0.8 * s);
            float d = length(p - c);
            float disc = smoothstep(rr + aa, rr * (1.0 - blur), d);
            float rim = smoothstep(rr * 0.7, rr, d) * disc * 0.4 * (1.0 - blur);
            float fade = smoothstep(0.0, 0.18, life) * smoothstep(1.0, 0.78, life);
            float tw = 0.8 + 0.2 * sin(t * 0.5 + s * 50.0);
            col += pal[(uint)(s * 4.0) & 3] * (disc * 0.8 + rim) * alpha * fade * tw;
        }
    }
    return col;
}

// ---------------------------------------------------------------- Silk
float silkHeight(float2 p, float t)
{
    // Large, slow diagonal folds bent by low-frequency noise.
    float2 w = float2(snoise(p * 0.7 + float2(0.0, t)), snoise(p * 0.7 + float2(4.1, -t * 0.8)));
    float u = dot(p, float2(0.55, 0.85)) + 0.55 * w.x + 0.12 * snoise(p * 1.6 - t * 0.5);
    return 0.5 + 0.5 * sin(u * 5.0 + w.y * 1.2);
}

float3 sceneSilk(float2 p, float t, float aa, float aspect)
{
    float tt = t * 0.025;
    float h = silkHeight(p, tt);
    // Analytic-ish normal from two extra taps (smooth at any resolution).
    const float e = 0.004;
    float hx = silkHeight(p + float2(e, 0), tt);
    float hy = silkHeight(p + float2(0, e), tt);
    float3 n = normalize(float3(-(hx - h) / e * 0.16, -(hy - h) / e * 0.16, 1.0));
    float3 l = normalize(float3(-0.45, 0.6, 0.65));
    float diff = saturate(dot(n, l));
    float spec = pow(saturate(dot(reflect(-l, n), float3(0, 0, 1))), 10.0);

    float tint = 0.5 + 0.5 * snoise(p * 0.5 + tt * 0.6);
    float3 base = lerp(hexc(0xEDC8D3), hexc(0xCEC4F0), tint);
    float3 col = base * (0.52 + 0.55 * diff) * (0.9 + 0.14 * h) + hexc(0xFFF4F0) * spec * 0.28;
    return col;
}

// ---------------------------------------------------------------- Ripple (halftone bubbles)
float rippleField(float2 q, float t, float aspect)
{
    float f = 0;
    [unroll] for (int i = 0; i < 4; i++)
    {
        float fi = i;
        float2 h = hash21(fi * 3.7 + 1.3);
        float2 e = float2((h.x - 0.5) * aspect * 0.8, (h.y - 0.5) * 0.7);
        e += 0.06 * float2(sin(t * 0.05 + fi), cos(t * 0.04 + fi * 2.0));
        float d = length(q - e);
        [unroll] for (int k = 0; k < 2; k++)
        {
            // Rings that are born at each source, spread out and fade.
            float life = frac(t * 0.04 + fi * 0.27 + k * 0.5);
            float R = life * 1.3;
            float w = (d - R) / (0.06 + 0.05 * life);
            f += exp(-w * w) * (1.0 - life) * (1.0 - life) * 0.9;
        }
    }
    // A slow, broad swell underneath.
    f += 0.22 * (0.5 + 0.5 * sin(q.x * 2.6 + q.y * 1.7 + t * 0.12)) * (0.5 + 0.5 * snoise(q * 0.8 + t * 0.02));
    return f;
}

float3 sceneRipple(float2 p, float t, float aa, float aspect)
{
    float y = p.y + 0.5;
    float3 col = lerp(hexc(0x07152B), hexc(0x0C2445), smoothstep(0.0, 1.0, y));
    float cell = 1.0 / 44.0;
    float2 id = floor(p / cell);
    float2 cc = (id + 0.5) * cell;
    float f = saturate(rippleField(cc, t, aspect));
    float r = cell * 0.5 * lerp(0.14, 0.9, f);
    float d = length(p - cc);
    float dotm = smoothstep(r + aa, r - aa, d);
    float3 dc = lerp(hexc(0x1D4C9C), hexc(0x9ADCFF), f * f);
    col = lerp(col, dc, dotm * lerp(0.45, 1.0, f));
    col += hexc(0x2F7BEA) * 0.07 * saturate(rippleField(p, t, aspect)); // soft glow between dots
    return col;
}

// ---------------------------------------------------------------- Grid (an alien sea of small squares)
// Works in pixel space so every square is crisp. Squares ride slow wave trains: they orbit,
// swell and brighten as waves pass, so the lattice shifts and breathes like something alive.

// Smooth ramp through 5 colours, v in 0..1.
float3 ramp5(float v, float3 c0, float3 c1, float3 c2, float3 c3, float3 c4)
{
    v = saturate(v) * 4.0;
    float3 c = lerp(c0, c1, saturate(v));
    c = lerp(c, c2, saturate(v - 1.0));
    c = lerp(c, c3, saturate(v - 2.0));
    return lerp(c, c4, saturate(v - 3.0));
}

// 3D simplex noise (Ashima Arts / Stefan Gustavson, MIT).
float4 mod289(float4 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
float4 permute(float4 x) { return mod289(((x * 34.0) + 1.0) * x); }
float4 taylorInvSqrt(float4 r) { return 1.79284291400159 - 0.85373472095314 * r; }
float snoise3(float3 v)
{
    const float2 C = float2(1.0 / 6.0, 1.0 / 3.0);
    const float4 D = float4(0.0, 0.5, 1.0, 2.0);
    float3 i = floor(v + dot(v, C.yyy));
    float3 x0 = v - i + dot(i, C.xxx);
    float3 g = step(x0.yzx, x0.xyz);
    float3 l = 1.0 - g;
    float3 i1 = min(g.xyz, l.zxy);
    float3 i2 = max(g.xyz, l.zxy);
    float3 x1 = x0 - i1 + C.xxx;
    float3 x2 = x0 - i2 + C.yyy;
    float3 x3 = x0 - D.yyy;
    i = mod289(i);
    float4 p = permute(permute(permute(i.z + float4(0.0, i1.z, i2.z, 1.0)) + i.y + float4(0.0, i1.y, i2.y, 1.0))
                       + i.x + float4(0.0, i1.x, i2.x, 1.0));
    float n_ = 0.142857142857;
    float3 ns = n_ * D.wyz - D.xzx;
    float4 j = p - 49.0 * floor(p * ns.z * ns.z);
    float4 x_ = floor(j * ns.z);
    float4 y_ = floor(j - 7.0 * x_);
    float4 x = x_ * ns.x + ns.yyyy;
    float4 y = y_ * ns.x + ns.yyyy;
    float4 h = 1.0 - abs(x) - abs(y);
    float4 b0 = float4(x.xy, y.xy);
    float4 b1 = float4(x.zw, y.zw);
    float4 s0 = floor(b0) * 2.0 + 1.0;
    float4 s1 = floor(b1) * 2.0 + 1.0;
    float4 sh = -step(h, 0.0);
    float4 a0 = b0.xzyw + s0.xzyw * sh.xxyy;
    float4 a1 = b1.xzyw + s1.xzyw * sh.zzww;
    float3 p0 = float3(a0.xy, h.x);
    float3 p1 = float3(a0.zw, h.y);
    float3 p2 = float3(a1.xy, h.z);
    float3 p3 = float3(a1.zw, h.w);
    float4 norm = taylorInvSqrt(float4(dot(p0, p0), dot(p1, p1), dot(p2, p2), dot(p3, p3)));
    p0 *= norm.x; p1 *= norm.y; p2 *= norm.z; p3 *= norm.w;
    float4 m = max(0.6 - float4(dot(x0, x0), dot(x1, x1), dot(x2, x2), dot(x3, x3)), 0.0);
    m = m * m;
    return 42.0 * dot(m * m, float4(dot(p0, x0), dot(p1, x1), dot(p2, x2), dot(p3, x3)));
}

// ---------------------------------------------------------------- Grid field (3D, sliced)
// The screen is a 2D slice through a 3D volume that holds a few wave sources and some noise.
// The slice slowly drifts, tilts and turns, so rings appear, grow, shrink and vanish as waves
// cross it; the squares follow the contours of the combined field, so meeting waves merge,
// split and reconnect instead of passing through each other.

float3 gridSlice(float2 q, float aspect, float t)
{
    float2 c = (q - float2(aspect * 0.5, 0.5)) * 1.1;
    float3 p = float3(c, 0.22 * sin(t * 0.011));
    float ry = 0.35 * sin(t * 0.0075), rx = 0.45 * sin(t * 0.0057 + 1.0), rz = t * 0.0035;
    float cz = cos(rz), sz = sin(rz), cx = cos(rx), sx = sin(rx), cy = cos(ry), sy = sin(ry);
    p = float3(p.x * cz - p.y * sz, p.x * sz + p.y * cz, p.z);
    p = float3(p.x, p.y * cx - p.z * sx, p.y * sx + p.z * cx);
    p = float3(p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy);
    return p;
}

// Where singularity k is at Grid time t. gridSeed picks a random environment per lull (layout,
// orbit and wave phases); 0 is the classic layout. Mirrored in gfx.cpp (GridScreenPeak).
float3 gridSource(int k, float t)
{
    float fk = k, sd = gridSeed;
    float3 s = float3(hash21(fk * 3.7 + 1.0 + sd * 13.1) - 0.5, hash11(fk * 5.3 + 2.0 + sd * 7.7) - 0.5) * float3(1.7, 1.1, 0.9);
    return s + 0.28 * float3(sin(t * (0.025 + 0.008 * fk) + fk + sd * 3.1), cos(t * (0.02 + 0.007 * fk) + 2.0 * fk + sd * 5.3),
                             sin(t * 0.016 + 3.0 * fk + sd * 2.3));
}

float gridVolume(float3 p, float t)
{
    float F = 0;
    float I = gridIntensity, sd = gridSeed;
    int n = (int)clamp(gridSources, 1.0, 8.0);
    [loop] for (int k = 0; k < n; k++)
    {
        float fk = k;
        float d = length(p - gridSource(k, t));
        // Now and then a source flares: a short, strong burst (more often and stronger with intensity).
        float flare = 1.0 + I * 2.4 * pow(saturate(0.5 + 0.5 * sin(t * (0.05 + 0.013 * fk) + fk * 2.1 + sd * 1.9)), 10.0);
        F += (0.8 + 0.5 * I) * flare * sin(d * (8.0 + 1.6 * fk) - t * (0.22 + 0.04 * fk) + fk * 1.7 + sd * 4.7) / (1.0 + 2.2 * d);
    }
    return F + lerp(0.35, 0.95, I) * snoise3(p * 1.35 + float3(sd * 17.3, sd * 11.1, t * 0.02));
}

// One texel per square: x = colour height, y = size (0..1; above 1 = a breaking crest),
// zw = displacement in cells.
float4 PSGridField(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float nh = normH > 0.0 ? normH : res.y;
    float cellPx = max(3.0, normH > 0.0 ? nh / 72.0 : floor(nh / 72.0));
    float aspect = res.x / nh;
    float t = time;
    float2 id = floor(pos.xy) - 1.0; // the texture has a one-cell border for neighbours
    float2 q = (id + 0.5) * cellPx / nh;

    // Contour bands of the combined field: c peaks along iso-lines that bend and reconnect.
    // While a giant breaks, the whole surface folds harder.
    const float e = 0.012;
    float bands = lerp(1.6, 3.6, saturate(gridIntensity + 0.45 * gridSurge));
    float F = gridVolume(gridSlice(q, aspect, t), t);
    float Fx = gridVolume(gridSlice(q + float2(e, 0.0), aspect, t), t);
    float Fy = gridVolume(gridSlice(q + float2(0.0, e), aspect, t), t);
    float c = 0.5 + 0.5 * cos(F * bands);
    float cx = 0.5 + 0.5 * cos(Fx * bands), cy = 0.5 + 0.5 * cos(Fy * bands);

    // Superwaves are no separate thing: they are this same field where several singularities'
    // crests arrive together and pile up past the breaking height (gridBreak, scaled like the
    // field's peaks, √N). There the squares swell into foam; equally deep troughs draw back.
    float B = gridBreak, s = max(0.5, sqrt(clamp(gridSources, 1.0, 8.0) / 5.0));
    float U = B > 0.0 ? smoothstep(B - 0.25 * s, B + 0.9 * s, F) : 0.0;
    float Tr = B > 0.0 ? smoothstep(B - 0.3 * s, B + 0.7 * s, -F) : 0.0;
    float swell = saturate(0.14 + 0.86 * smoothstep(0.25, 0.95, c)) * (1.0 - 0.75 * Tr);

    // Squares drift up the slope toward the ridges and bunch up along them. On a breaking crest
    // they close ranks and gather onto it, so it reads as one solid wall of water.
    float2 g = float2(cx - c, cy - c) / e;
    float2 disp = g * lerp(0.07, 0.17, gridIntensity) * (1.0 - 0.75 * U) + float2(Fx - F, Fy - F) / e * 0.02 * U;
    float len = length(disp);
    if (len > 0.75) disp *= 0.75 / len;

    float hgt = 0.5 + 0.5 * snoise3(float3(q * 0.9, t * 0.02 + 5.0));
    return float4(hgt, max(swell, U) + 1.2 * U, disp);
}

Texture2D<float4> gridField : register(t1);

// Grid's palette. Four colour families (w = violet, teal, rose weights over the base blue) in two
// looks: Night, deep and glowing; Day, a pale misty sky with deeper pastel ridges. A breaking
// crest turns to foam at night and to warm sunlight by day.
float3 gridBackdrop(float3 w, float day)
{
    float3 n = lerp(lerp(lerp(hexc(0x06102A), hexc(0x0B0728), w.x), hexc(0x05171A), w.y), hexc(0x170823), w.z);
    float3 d = lerp(lerp(lerp(hexc(0xDFE8F8), hexc(0xE8E1F7), w.x), hexc(0xDDF1EC), w.y), hexc(0xF5E4EE), w.z);
    return lerp(n, d, day);
}

float3 gridSquare(float v, float crest, float3 w, float day)
{
    float3 n = 0, d = 0;
    [branch] if (day < 0.999)
    {
        n = lerp(lerp(lerp(ramp5(v, hexc(0x0A1D40), hexc(0x14336B), hexc(0x22559C), hexc(0x3D82CC), hexc(0x8FCBF2)),
                           ramp5(v, hexc(0x170F3A), hexc(0x2A1C66), hexc(0x47319A), hexc(0x7357CB), hexc(0xB7A2F4)), w.x),
                      ramp5(v, hexc(0x072328), hexc(0x0E3F44), hexc(0x16676A), hexc(0x2AA08E), hexc(0x86EBCF)), w.y),
                 ramp5(v, hexc(0x220B2C), hexc(0x3E1650), hexc(0x662A7A), hexc(0x9E4C9E), hexc(0xEBA6DA)), w.z);
        float3 foam = lerp(lerp(lerp(hexc(0xCBE9FF), hexc(0xDCD0FF), w.x), hexc(0xC4F7E6), w.y), hexc(0xF8D8EE), w.z);
        n = lerp(n, foam, 0.8 * crest);
    }
    [branch] if (day > 0.001)
    {
        d = lerp(lerp(lerp(ramp5(v, hexc(0xC9D7F3), hexc(0xA7BDEE), hexc(0x839FE5), hexc(0x6482D8), hexc(0x4F6BC6)),
                           ramp5(v, hexc(0xDACDF3), hexc(0xC1ACEC), hexc(0xA58AE1), hexc(0x8A6DD2), hexc(0x7259BE)), w.x),
                      ramp5(v, hexc(0xC4E8DF), hexc(0x99D7C9), hexc(0x6CC3B1), hexc(0x47A896), hexc(0x32907F)), w.y),
                 ramp5(v, hexc(0xF2D2E4), hexc(0xE8B2D0), hexc(0xDB90BB), hexc(0xC972A6), hexc(0xB25A90)), w.z);
        float3 sun = lerp(hexc(0xFFD3A6), hexc(0xFFE4BD), w.x);
        d = lerp(d, sun, 0.85 * crest);
    }
    return lerp(n, d, day);
}

float3 sceneGrid(float2 fragPx, float t)
{
    float nh = normH > 0.0 ? normH : res.y; // framing (see PSMain)
    float cellPx = max(3.0, normH > 0.0 ? nh / 72.0 : floor(nh / 72.0)); // whole pixels on the lock screen
    float2 base = floor(fragPx / cellPx);
    uint fw, fh;
    gridField.GetDimensions(fw, fh);

    // Large regions drift between four colour families: blue, violet, teal-green and a rarer rose.
    float2 qp = fragPx / nh;
    float3 w = float3(
        smoothstep(0.38, 0.84, 0.5 + 0.5 * snoise(qp * 0.55 + float2(-t * 0.008, t * 0.005) + 9.0)),
        smoothstep(0.55, 0.90, 0.5 + 0.5 * snoise(qp * 0.7 + float2(t * 0.007, t * 0.004) + 31.0)),
        smoothstep(0.66, 0.95, 0.5 + 0.5 * snoise(qp * 0.6 + float2(-t * 0.006, -t * 0.007) + 57.0)));
    float day = gridDay;

    float3 bg = gridBackdrop(w, day);
    float3 col = bg;
    float cover = 0;

    // Squares can drift up to most of a cell, so look at this cell and its neighbours.
    [unroll] for (int dy = -1; dy <= 1; dy++)
    [unroll] for (int dx = -1; dx <= 1; dx++)
    {
        float2 id = base + float2(dx, dy);
        int2 tc = clamp(int2(id) + 1, int2(0, 0), int2(fw, fh) - 1);
        float4 f = gridField.Load(int3(tc, 0));
        float2 c = (id + 0.5) * cellPx + f.zw * cellPx;
        // Size follows the field: big on the ridges, small (never gone) between them; a
        // superwave's crest (f.y above 1) swells the squares almost into each other.
        float sw = saturate(f.y), crest = saturate(f.y - 1.0);
        float sz = cellPx * (lerp(0.28, 0.76, sw) + 0.16 * crest);
        float2 d = abs(fragPx - c) - sz * 0.5;
        float m = smoothstep(0.7, -0.7, max(d.x, d.y));
        if (m > cover)
        {
            // Colour from a broad field with a raised floor (no black holes); ridges glow a bit.
            float v = saturate(0.3 + 0.62 * smoothstep(0.05, 0.95, f.x) + 0.3 * (sw - 0.35) + 0.4 * crest);
            col = lerp(bg, gridSquare(v, crest, w, day), m);
            cover = m;
        }
    }
    return col;
}

float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    // Framing: by default the scene is centred and scaled to the canvas height. The tray panel
    // renders a taller canvas with the preview's framing so the scene continues below it.
    float nh = normH > 0.0 ? normH : res.y;
    float2 org = normH > 0.0 ? origin : 0.5 * res;
    float aspect = res.x / nh;
    float2 p = (pos.xy - org) / nh;
    p.y = -p.y;
    float aa = 1.5 / nh;
    float t = time;

    float3 c;
#if SCENE == 0
    c = sceneAurora(p, t, aa, aspect);
#elif SCENE == 1
    c = sceneDrift(p, t, aa, aspect);
#elif SCENE == 2
    c = sceneTide(p, t, aa, aspect);
#elif SCENE == 3
    c = sceneBreathe(p, t, aa, aspect);
#elif SCENE == 4
    c = sceneLanterns(p, t, aa, aspect);
#elif SCENE == 5
    c = sceneSilk(p, t, aa, aspect);
#elif SCENE == 6
    c = sceneRipple(p, t, aa, aspect);
#else
    c = sceneGrid(pos.xy, t);
#endif

    // Soft vignette (barely there on Grid's day look, where darkened corners would turn grey).
    float2 vq = pos.xy / res - 0.5;
#if SCENE == 7
    c *= 1.0 - lerp(0.18, 0.05, gridDay) * dot(vq, vq) * 2.0;
#else
    c *= 1.0 - 0.18 * dot(vq, vq) * 2.0;
#endif
    return float4(toSrgb(c), 1.0);
}
