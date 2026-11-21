// HLSL source of the SDL_GPU GS backend (gs_gpu_backend.cpp) for Direct3D 12,
// compiled at build time by DXC (Shader Model 6.0, HLSL 2021) into DXIL
// headers. A line by line translation of gs_gpu_shaders_msl.h: keep the two
// in step. Variants of gs_fs come from GS_SLOW and GS_DEPTH_OUT (-D).
//
// Bindings follow SDL_gpu.h for HLSL: vertex uniforms b0 space1; fragment
// textures t0-3 and samplers s0-3 in space2, fragment uniforms b0 space3;
// vertex attribute n is TEXCOORDn.

#ifndef GS_SLOW
#define GS_SLOW 0
#endif
#ifndef GS_DEPTH_OUT
#define GS_DEPTH_OUT 0
#endif

// Android (Vulkan): SPIR-V from the NDK's glslc, whose HLSL has no select().
#if defined(GS_SPIRV)
#define select(c, a, b) ((c) ? (a) : (b))
#endif

struct VSIn
{
    float4 pos : TEXCOORD0;      // x, y in GS pixels of the target, z in [0,1]
    float4 color : TEXCOORD1;    // 0-255
    float4 tex : TEXCOORD2;      // s, t, q (or u, v) and fog
    float4 uvBounds : TEXCOORD3; // the primitive's texel range: u min, v min, u max, v max
};

// Colour and STQ are interpolated linearly on screen, as the GS does (the
// GS-path vertices have w = 1; native draws give the real w for clipping).
struct VSOut
{
    float4 position : SV_Position;
    noperspective float4 color : TEXCOORD0;
    noperspective float4 tex : TEXCOORD1;
    nointerpolation float4 uvBounds : TEXCOORD2;
};

cbuffer VSUniforms : register(b0, space1)
{
    float4 vsNdc; // 2 / target width, 2 / target height (GS pixels)
};

VSOut gs_vs(VSIn i)
{
    VSOut o;
    o.position = float4(i.pos.x * vsNdc.x - 1.0, 1.0 - i.pos.y * vsNdc.y, i.pos.z, 1.0);
    o.color = i.color;
    o.tex = i.tex;
    o.uvBounds = i.uvBounds;
    return o;
}

// A native draw (runtime/gs/gs_native.h): a mesh's vertices transformed as
// the VU1 program it stands for would (GS window X, Y = x'/w', y'/w'; Z =
// z'/w' clamped at 0; STQ = (s, t, 1) / w'; the colour as given; the FOG
// register's fog), given to the rasterizer in clip space so the GPU clips
// what the GS could not take. Uniforms: struct GpuNativeUniforms.
struct NVSIn
{
    float3 pos : TEXCOORD0;
    float2 st : TEXCOORD1;
    uint4 color : TEXCOORD2;
    float4 stBounds : TEXCOORD3;
    int4 normal : TEXCOORD4; // object space, 32768 = 1.0
};

struct NVSUniforms
{
    float4 m0, m1, m2, m3; // rows: [x y z 1] * m (GSNativeDraw::transform)
    float4 view;           // XYOFFSET x, y (pixels), 2 / target width, 2 / target height
    float4 misc;           // 1 / Z scale, texture width, texture height, fog
    float4 opts;           // x: 1 = colour interpolated in perspective (divided by Q in the fragment stage),
                           // y: alpha (< 0: the vertex's), z: normal offset / 32768,
                           // w: S/T transformed by uvm from (s, t, 1) (1) or from the normal / 32768 (2)
    float4 uvm0;           // S, T = i.x * uvm0.xy + i.y * uvm0.zw + i.z * uvm1.xy + uvm1.zw
    float4 uvm1;
    float4 c0, c1, c2, c3; // rows: [x y z 1] * clip, the VU CLIP flags' volume (GSNativeDraw::clipTransform)
    float4 ext;            // x: GSNativeDraw::dropFlags, y: 1 = X, Y clamped to 0..4095
    // The lights (a lit rigid draw's GSNativeLights, a skinned one's GSNativeSkinning).
    float4 lightDir[3];    // row k: component k of lights 0, 1, 2
    float4 lightColour[4]; // lights 0, 1, 2, the ambient
    float4 light;          // x: 1 = a rigid mesh lit from its normals, y: its colour's scale, z: its alpha
};
#if defined(GS_SPIRV)
// (glslang: a ConstantBuffer's struct cannot also be a member of SVSUniforms.)
cbuffer NVSBlock : register(b0, space1)
{
    NVSUniforms nvu;
};
#else
ConstantBuffer<NVSUniforms> nvu : register(b0, space1);
#endif

// A native draw's vertex output: a fragment's, and the clip distance that
// leaves out a triangle the microprogram would not draw (one of its
// vertices -1, the others 0: nothing of it is >= 0 but an edge).
struct NVSOut
{
    float4 position : SV_Position;
    noperspective float4 color : TEXCOORD0;
    noperspective float4 tex : TEXCOORD1;
    nointerpolation float4 uvBounds : TEXCOORD2;
    float clip : SV_ClipDistance0;
};

// The VU's CLIPw.xyz flags against |max(w, 0)| (gsNativeClipFlags).
uint clipFlags(float4 c)
{
    const float w = max(c.w, 0.0);
    return (c.x > w ? 1u : 0u) | (c.x < -w ? 2u : 0u) | (c.y > w ? 4u : 0u) | (c.y < -w ? 8u : 0u) |
           (c.z > w ? 16u : 0u) | (c.z < -w ? 32u : 0u);
}

// The common tail of the native vertex shaders: h = the GS window position
// (x', y', z', w'), clipped as GS pixels relative to XYOFFSET; colour, STQ.
NVSOut nativeOut(float4 h, float4 c, float2 st, float4 b, float q, float drop, float4 view, float4 misc,
                 bool perspectiveColour)
{
    NVSOut o;
    const float w = h.w;
    o.position = float4(h.x * view.z - (view.x * view.z + 1.0) * w,
                        (view.y * view.w + 1.0) * w - h.y * view.w,
                        max(h.z, 0.0) * misc.x,
                        w);
    o.color = perspectiveColour ? c * q : c;
    o.tex = float4(st.x * q, st.y * q, q, misc.w);
    o.uvBounds = b * float4(misc.y, misc.z, misc.y, misc.z);
    o.clip = drop;
    return o;
}

NVSOut gs_native_vs(NVSIn i)
{
    const float3 p = i.pos + float3(i.normal.xyz) * nvu.opts.z;
    float4 h = p.x * nvu.m0 + p.y * nvu.m1 + p.z * nvu.m2 + nvu.m3;
    if (nvu.ext.y != 0.0 && h.w > 0.0)
        h.xy = clamp(h.xy / h.w, 0.0, 4095.0) * h.w;
    float drop = 0.0;
    if (nvu.ext.x != 0.0)
    {
        const float4 c = p.x * nvu.c0 + p.y * nvu.c1 + p.z * nvu.c2 + nvu.c3;
        drop = (clipFlags(c) & uint(nvu.ext.x)) != 0u ? -1.0 : 0.0;
    }
    float4 c = float4(i.color);
    if (nvu.light.x != 0.0)
    {
        // A lit rigid mesh (gsNativeLightVertex): FTOI0 of min(scale l, 255), the GS keeps the low byte.
        const float3 n = float3(i.normal.xyz) / 32768.0;
        const float3 d = max((nvu.lightDir[0].xyz * n.x + nvu.lightDir[1].xyz * n.y) + nvu.lightDir[2].xyz * n.z, 0.0);
        const float3 l = ((nvu.lightColour[3].xyz + nvu.lightColour[0].xyz * d.x) + nvu.lightColour[1].xyz * d.y) +
                         nvu.lightColour[2].xyz * d.z;
        const int3 ic = int3(max(trunc(min(l * nvu.light.y, 255.0)), -2147483520.0));
        c = float4(float3(ic & int3(255, 255, 255)), nvu.light.z);
    }
    if (nvu.opts.y >= 0.0)
        c.a = nvu.opts.y;
    float2 st = i.st;
    float4 b = i.stBounds;
    if (nvu.opts.w > 1.5)
    {
        const float3 n = float3(i.normal.xyz) / 32768.0;
        st = n.x * nvu.uvm0.xy + n.y * nvu.uvm0.zw + n.z * nvu.uvm1.xy + nvu.uvm1.zw;
        b = float4(-1.0e7, -1.0e7, 1.0e7, 1.0e7);
    }
    else if (nvu.opts.w != 0.0)
    {
        st = st.x * nvu.uvm0.xy + st.y * nvu.uvm0.zw + nvu.uvm1.xy + nvu.uvm1.zw;
        const float2 b0 = b.x * nvu.uvm0.xy + b.y * nvu.uvm0.zw + nvu.uvm1.xy + nvu.uvm1.zw;
        const float2 b1 = b.z * nvu.uvm0.xy + b.w * nvu.uvm0.zw + nvu.uvm1.xy + nvu.uvm1.zw;
        b = float4(min(b0, b1), max(b0, b1));
    }
    return nativeOut(h, c, st, b, 1.0 / h.w, drop, nvu.view, nvu.misc, nvu.opts.x != 0.0);
}

// A skinned native draw (GSNativeSkinning; AI's VU1 0x08 / 0x0C programs):
// each vertex moved by up to four bones of the draw's palette, lit by three
// directional lights and an ambient or with its own colour, then as
// gs_native_vs with STQ = (s, t, 1) (affine: GSNativeDraw::affineUV).
struct SVSIn
{
    float3 pos : TEXCOORD0;    // with the bones' addresses in the low mantissa bits, as VU1 gets it
    float2 st : TEXCOORD1;
    uint4 color : TEXCOORD2;
    float4 stBounds : TEXCOORD3;
    int4 normal : TEXCOORD4;   // 32768 = 1.0 (weights in the low bits, as VU1 reads it)
    uint4 bone : TEXCOORD5;    // GSNativeSkinWeights: palette rows
    uint4 weight : TEXCOORD6;  // sixteenths
};

struct SVSUniforms
{
    NVSUniforms n;         // with the lights
    float4 skin;           // x: 1 = lit, y: the colour's cap (255 as VU1, 128 the Xbox lighting), z: 1 = alpha lit too
    float4 palette[128];   // VU1 0..127
};
ConstantBuffer<SVSUniforms> svu : register(b0, space1);

NVSOut gs_native_skin_vs(SVSIn i)
{
    float3 p = float3(0.0, 0.0, 0.0);
    [unroll] for (int b = 0; b < 4; ++b)
    {
        const uint a = min(i.bone[b], 124u);
        const float k = float(i.weight[b]) / 16.0;
        p += (((svu.palette[a + 3].xyz + svu.palette[a].xyz * i.pos.x) + svu.palette[a + 1].xyz * i.pos.y) +
              svu.palette[a + 2].xyz * i.pos.z) * k;
    }
    float4 c = float4(i.color);
    if (svu.skin.x != 0.0)
    {
        const uint a = min(i.bone.x, 124u);
        const float3 n = float3(i.normal.xyz) / 32768.0;
        const float3 nn = (svu.palette[a].xyz * n.x + svu.palette[a + 1].xyz * n.y) + svu.palette[a + 2].xyz * n.z;
        const float3 d = max((svu.n.lightDir[0].xyz * nn.x + svu.n.lightDir[1].xyz * nn.y) + svu.n.lightDir[2].xyz * nn.z, 0.0);
        const float3 l = ((svu.n.lightColour[3].xyz + svu.n.lightColour[0].xyz * d.x) + svu.n.lightColour[1].xyz * d.y) +
                         svu.n.lightColour[2].xyz * d.z;
        // FTOI0 of min(128 l, 255); the GS keeps the low byte.
        const int3 ic = int3(max(trunc(min(l * 128.0, svu.skin.y)), -2147483520.0));
        c = float4(float3(ic & int3(255, 255, 255)), 128.0);
        if (svu.skin.z != 0.0)
        {
            // DM2's lit program: the alpha from the colours' w, not capped (FTOI0, the low byte).
            const float la = ((svu.n.lightColour[3].w + svu.n.lightColour[0].w * d.x) + svu.n.lightColour[1].w * d.y) +
                             svu.n.lightColour[2].w * d.z;
            c.w = float(int(clamp(trunc(la * 128.0), -2147483520.0, 2147483520.0)) & 255);
        }
    }
    const float4 h = p.x * svu.n.m0 + p.y * svu.n.m1 + p.z * svu.n.m2 + svu.n.m3;
    float drop = 0.0;
    if (svu.n.ext.x != 0.0)
    {
        const float4 cc = p.x * svu.n.c0 + p.y * svu.n.c1 + p.z * svu.n.c2 + svu.n.c3;
        drop = (clipFlags(cc) & uint(svu.n.ext.x)) != 0u ? -1.0 : 0.0;
    }
    return nativeOut(h, c, i.st, i.stBounds, svu.n.ext.z != 0.0 ? 1.0 : 1.0 / h.w, drop, svu.n.view, svu.n.misc,
                     svu.n.opts.x != 0.0);
}

// The fragment uniforms are laid out as struct GpuFragmentUniforms in
// gs_gpu_backend.cpp.
struct FSUniforms
{
    uint4 cfg0; // x flags, y texture kind (0 RGBA, 1 indexed, 2 render target, 3 replacement), z texture function bits, w alpha test bits
    uint4 cfg1; // x wrap modes, y minU | maxU << 16, z minV | maxV << 16, w texW | texH << 16
    uint4 cfg2; // x ta0 | ta1 << 8 | aem << 16, y fog color, z FBMSK, w blend A | B << 2 | C << 4 | D << 6 | FIX << 8
    uint4 cfg3; // x source term, y output alpha mode, z target bits, w max Z
    float4 f0;  // x scale (horizontal), y/z texture origin in the source target (GS pixels), w Z scale (2^bits)
    float4 f1;  // x/y source target size (GS pixels), z vertical scale, w mipmap K (replacement: alpha for 1.0)
    float4 f2;  // replacement: the GS texels its picture covers, x/y origin, z/w size (z 0: the whole texture)
};
ConstantBuffer<FSUniforms> u : register(b0, space3);

Texture2D<float4> tex : register(t0, space2);
Texture2D<uint4> idx : register(t1, space2);
Texture2D<float4> pal : register(t2, space2);
Texture2D<float4> dst : register(t3, space2);
SamplerState s0 : register(s0, space2);
SamplerState s1 : register(s1, space2);
SamplerState s2 : register(s2, space2);
SamplerState s3 : register(s3, space2);

// cfg0.x
#define F_TME 1u
#define F_FGE 2u
#define F_TINT 4u // diagnostics: replacement pictures tinted red (ZX_GS_REPL_TINT=1)
#define F_TINT2 8u // and indexed textures without one blue (ZX_GS_REPL_TINT=2)
#define F_NATIVE_TINT 16u // diagnostics: native draws tinted green (ZX_NATIVE_TINT=1)
#define F_PERSP_COLOR 32u // native draw: colour / Q is the vertex colour in perspective
// A constant colour (flat shading, or the same at every vertex) feeding a
// counter: rounded, not truncated, so the interpolation's float error
// cannot take one off it (the skater shadow's volumes, gs_quality Shadows)
#define F_EXACT_COLOR 0x40000u
#define F_MAG_NEAREST 0x10000u // kind 3, a PS2 texture's mipmapped copy the game magnifies NEAREST
#define F_MIP_INSET 0x20000u // kind 3, a native draw: clamps follow the mip level (f1.x: the sampler's anisotropy)
// cfg0.z
#define TF_TFX(z) ((z) & 3u)
#define TF_TCC(z) (((z) >> 2) & 1u)
#define TF_COORD(z) (((z) >> 4) & 3u)
#define TF_LINEAR(z) (((z) >> 8) & 1u)
#define TF_TEXA(z) (((z) >> 12) & 3u)
#define TF_SRC16(z) (((z) >> 14) & 1u)
// cfg0.z, mipmapping (GSMipInfo): MXL, lowest level present, LCM, MMAG,
// MMIN texel filter, MMIN level blend, L
#define TF_MXL(z) (((z) >> 16) & 7u)
#define TF_MIPMIN(z) (((z) >> 19) & 7u)
#define TF_LCM(z) (((z) >> 22) & 1u)
#define TF_MAGLIN(z) (((z) >> 23) & 1u)
#define TF_MINLIN(z) (((z) >> 24) & 1u)
#define TF_MIPLIN(z) (((z) >> 25) & 1u)
#define TF_MIPL(z) (((z) >> 26) & 3u)
// cfg3.z
#define T_16BIT 1u
#define T_COLCLAMP 2u
#define T_PABE 4u
#define T_FBA 8u
#define T_DATE 16u
#define T_DATM 32u
#define T_KEEP_ALPHA 64u
#define T_CT24 128u
#define T_ABE 256u

// MSL's round() (half away from zero); HLSL's rounds half to even.
float roundAway(float x)
{
    const float t = trunc(x);
    return abs(x - t) >= 0.5 ? t + sign(x) : t;
}

float4 roundAway4(float4 x)
{
    return float4(roundAway(x.x), roundAway(x.y), roundAway(x.z), roundAway(x.w));
}

uint4 to8(float4 c)
{
    return uint4(clamp(roundAway4(c * 255.0), 0.0, 255.0));
}

uint4 quantize16(uint4 c)
{
    return uint4(c.rgb & 0xF8u, (c.a & 0x80u) != 0u ? 0x80u : 0u);
}

uint2 size2(Texture2D<float4> t)
{
    uint w, h;
    t.GetDimensions(w, h);
    return uint2(w, h);
}

uint4 applyTexa(uint4 c, uint cls)
{
    if (cls == 0u)
        return c;
    const uint ta0 = u.cfg2.x & 0xFFu;
    const uint ta1 = (u.cfg2.x >> 8) & 0xFFu;
    const bool aem = ((u.cfg2.x >> 16) & 1u) != 0u;
    const bool rgb0 = c.r == 0u && c.g == 0u && c.b == 0u;
    if (cls == 1u)
        c.a = (aem && rgb0) ? 0u : ta0;
    else if ((c.a & 0x80u) != 0u)
        c.a = ta1;
    else
        c.a = (aem && rgb0) ? 0u : ta0;
    return c;
}

int wrapCoord(int c, int size, uint mode, int lo, int hi)
{
    switch (mode)
    {
    case 0u: return int(uint(c) & uint(size - 1));
    case 1u: return clamp(c, 0, size - 1);
    case 2u: return min(max(c, lo), hi);
    default: return int((uint(c) & uint(lo)) | uint(hi));
    }
}

// One texel of mipmap level `lod` at integer GS texture coordinates of
// that level (before wrapping; level k: sizes and CLAMP regions >> k).
uint4 fetchTexel(int2 t, uint lod)
{
    const int texW = max(int(u.cfg1.w & 0xFFFFu) >> lod, 1);
    const int texH = max(int(u.cfg1.w >> 16) >> lod, 1);
    const int x = wrapCoord(t.x, texW, u.cfg1.x & 3u, int(u.cfg1.y & 0xFFFFu) >> lod, int(u.cfg1.y >> 16) >> lod);
    const int y = wrapCoord(t.y, texH, (u.cfg1.x >> 2) & 3u, int(u.cfg1.z & 0xFFFFu) >> lod, int(u.cfg1.z >> 16) >> lod);
    uint4 c;
    uint w, h, levels;
    if (u.cfg0.y == 1u)
    {
        idx.GetDimensions(lod, w, h, levels);
        const int2 q = int2(clamp(x, 0, int(w) - 1), clamp(y, 0, int(h) - 1));
        c = to8(pal.Load(int3(int(idx.Load(int3(q, lod)).r), 0, 0)));
    }
    else
    {
        tex.GetDimensions(lod, w, h, levels);
        const int2 p = int2(clamp(x, 0, int(w) - 1), clamp(y, 0, int(h) - 1));
        c = to8(tex.Load(int3(p, lod)));
    }
    return applyTexa(c, TF_TEXA(u.cfg0.z));
}

// A texel of a render target used as a texture, at the internal resolution.
uint4 fetchTarget(int2 p)
{
    const uint2 ts = size2(tex);
    p = clamp(p, int2(0, 0), int2(ts) - 1);
    uint4 c = to8(tex.Load(int3(p, 0)));
    if (TF_SRC16(u.cfg0.z) != 0u)
        c = quantize16(c);
    return applyTexa(c, TF_TEXA(u.cfg0.z));
}

uint lerp8(uint c00, uint c10, uint c01, uint c11, float fx, float fy)
{
    const float top = float(c00) + (float(c10) - float(c00)) * fx;
    const float bottom = float(c01) + (float(c11) - float(c01)) * fx;
    return uint(clamp(roundAway(top + (bottom - top) * fy), 0.0, 255.0));
}

uint4 lerp4(uint4 c00, uint4 c10, uint4 c01, uint4 c11, float fx, float fy)
{
    return uint4(lerp8(c00.r, c10.r, c01.r, c11.r, fx, fy),
                 lerp8(c00.g, c10.g, c01.g, c11.g, fx, fy),
                 lerp8(c00.b, c10.b, c01.b, c11.b, fx, fy),
                 lerp8(c00.a, c10.a, c01.a, c11.a, fx, fy));
}

uint4 sampleLevel(float2 uv, uint lod, bool bilinear)
{
    if (!bilinear)
        return fetchTexel(int2(uv), lod);
    const float2 s = uv - 0.5;
    const int2 p0 = int2(floor(s));
    const float2 f = s - float2(p0);
    return lerp4(fetchTexel(p0, lod), fetchTexel(p0 + int2(1, 0), lod),
                 fetchTexel(p0 + int2(0, 1), lod), fetchTexel(p0 + int2(1, 1), lod), f.x, f.y);
}

// GS mipmapping (gsMipSelect): LOD = (log2(1/|Q|) << L) + K, or K (LCM).
// LOD < 0 is MMAG on level 0; otherwise MMIN, with NEAREST_MIPMAP rounding
// LOD to a level and LINEAR_MIPMAP blending floor(LOD) and the next level.
uint4 sampleMip(float2 uv, float q)
{
    const uint tf = u.cfg0.z;
    const uint mxl = TF_MXL(tf);
    float lod = u.f1.w;
    if (TF_LCM(tf) == 0u)
        lod += -log2(max(abs(q), 1.0e-30)) * float(1u << TF_MIPL(tf));
    uint level = 0u, level2 = 0u;
    float frac = 0.0;
    bool bilinear;
    if (!(lod >= 0.0))
        bilinear = TF_MAGLIN(tf) != 0u;
    else
    {
        bilinear = TF_MINLIN(tf) != 0u;
        if (TF_MIPLIN(tf) == 0u)
        {
            level = uint(min(floor(lod + 0.5), float(mxl)));
            level2 = level;
        }
        else if (lod >= float(mxl))
        {
            level = mxl;
            level2 = mxl;
        }
        else
        {
            level = uint(floor(lod));
            level2 = level + 1u;
            frac = lod - floor(lod);
        }
    }
    // Levels below the lowest one present (level 0 in a render target).
    const uint lowest = TF_MIPMIN(tf);
    if (level < lowest)
    {
        level = lowest;
        frac = level2 > lowest ? frac : 0.0;
        level2 = max(level2, lowest);
    }
    const uint4 c0 = sampleLevel(uv / float(1u << level), level, bilinear);
    if (level2 == level || !(frac > 0.0))
        return c0;
    const uint4 c1 = sampleLevel(uv / float(1u << level2), level2, bilinear);
    return uint4(clamp(roundAway4(float4(c0) + (float4(c1) - float4(c0)) * frac), 0.0, 255.0));
}

uint4 sampleTexture(float2 uv)
{
    const bool bilinear = TF_LINEAR(u.cfg0.z) != 0u;
    if (u.cfg0.y == 2u)
    {
        // Render target: sample it at its own resolution.
        const float2 sp = (uv + u.f0.yz) * float2(u.f0.x, u.f1.z);
        if (!bilinear)
            return fetchTarget(int2(floor(sp)));
        const float2 s = sp - 0.5;
        const int2 p0 = int2(floor(s));
        const float2 f = s - float2(p0);
        return lerp4(fetchTarget(p0), fetchTarget(p0 + int2(1, 0)),
                     fetchTarget(p0 + int2(0, 1)), fetchTarget(p0 + int2(1, 1)), f.x, f.y);
    }
    return sampleLevel(uv, 0u, bilinear);
}

// A replacement picture (texture kind 3), as sampleReplacement in the MSL.
uint4 sampleReplacement(float2 uv, float2 dx, float2 dy, float4 bounds)
{
    const float2 texSize = float2(float(u.cfg1.w & 0xFFFFu), float(u.cfg1.w >> 16));
    const float2 size = float2(size2(tex));
    // A texture pack's picture of a region (texture_pack::Region) covers
    // that rectangle only.
    const float2 area = u.f2.z > 0.0 ? u.f2.zw : texSize;
    const float2 origin = u.f2.xy;
    const float2 px = area / size; // GS texels per picture texel
    float2 inset = 0.5 * px;
    // REGION_REPEAT cells (2^k texels at FIX: the backend checks the form).
    const float cellU = float(((u.cfg1.y & 0xFFFFu) & ((u.cfg1.w & 0xFFFFu) - 1u)) + 1u);
    const float cellV = float(((u.cfg1.z & 0xFFFFu) & ((u.cfg1.w >> 16) - 1u)) + 1u);
    if ((u.cfg0.x & F_MIP_INSET) != 0u)
    {
        // A native draw: half a texel of the coarsest level the filter
        // reads (trilinear: the next level), so lower levels stay inside
        // the primitive's range and the regions as level 0 does (with half
        // a level 0 texel they reach the neighbours, or wrap: lines down
        // palm fronds), and no level coarser than one texel over the range.
        float2 extent = bounds.zw - bounds.xy;
        if ((u.cfg1.x & 3u) == 2u)
            extent.x = min(extent.x, float((u.cfg1.y >> 16) + 1u) - float(u.cfg1.y & 0xFFFFu));
        if (((u.cfg1.x >> 2) & 3u) == 2u)
            extent.y = min(extent.y, float((u.cfg1.z >> 16) + 1u) - float(u.cfg1.z & 0xFFFFu));
        if ((u.cfg1.x & 3u) == 3u)
            extent.x = min(extent.x, cellU);
        if (((u.cfg1.x >> 2) & 3u) == 3u)
            extent.y = min(extent.y, cellV);
        // The level as an anisotropic filter of f1.x samples picks it.
        const float rx = length(dx / px), ry = length(dy / px);
        const float rmax = max(rx, ry), rmin = min(rx, ry);
        const float n = clamp(ceil(rmax / max(rmin, 1.0e-8)), 1.0, max(u.f1.x, 1.0));
        float lod = log2(max(rmax / n, 1.0e-8));
        const float cap = log2(max(min(extent.x / px.x, extent.y / px.y), 1.0));
        if (lod > cap)
        {
            const float f = exp2(cap - lod);
            dx *= f;
            dy *= f;
            lod = cap;
        }
        inset = 0.5 * px * exp2(ceil(max(lod, 0.0)));
    }
    // Inside the primitive's texel range and REGION_CLAMP's, half a
    // replacement texel in (no filtering across atlas neighbours).
    const float2 lo = bounds.xy + inset, hi = bounds.zw - inset;
    uv = select(hi < lo, (bounds.xy + bounds.zw) * 0.5, clamp(uv, lo, hi));
    if ((u.cfg1.x & 3u) == 2u)
    {
        const float a = float(u.cfg1.y & 0xFFFFu), b = float((u.cfg1.y >> 16) + 1u), mid = 0.5 * (a + b);
        uv.x = clamp(uv.x, min(a + inset.x, mid), max(b - inset.x, mid));
    }
    if (((u.cfg1.x >> 2) & 3u) == 2u)
    {
        const float a = float(u.cfg1.z & 0xFFFFu), b = float((u.cfg1.z >> 16) + 1u), mid = 0.5 * (a + b);
        uv.y = clamp(uv.y, min(a + inset.y, mid), max(b - inset.y, mid));
    }
    // REGION_REPEAT in a cell: wrapped there, the inset in (the cell's
    // middle if it is narrower).
    if ((u.cfg1.x & 3u) == 3u)
        uv.x = float(u.cfg1.y >> 16) + (2.0 * inset.x < cellU ? clamp(uv.x - cellU * floor(uv.x / cellU), inset.x, cellU - inset.x)
                                                              : 0.5 * cellU);
    if (((u.cfg1.x >> 2) & 3u) == 3u)
        uv.y = float(u.cfg1.z >> 16) + (2.0 * inset.y < cellV ? clamp(uv.y - cellV * floor(uv.y / cellV), inset.y, cellV - inset.y)
                                                              : 0.5 * cellV);
    float4 c;
    if ((u.cfg0.x & F_MAG_NEAREST) != 0u && max(length(dx * size / area), length(dy * size / area)) <= 1.0)
    {
        // Magnified NEAREST: the texel the GS would take (truncated).
        c = tex.Load(int3(int2(clamp(floor((uv - origin) * size / area), float2(0.0, 0.0), size - 1.0)), 0));
    }
    else
        c = tex.SampleGrad(s0, (uv - origin) / area, dx / area, dy / area);
    uint4 t = uint4(clamp(roundAway4(c * float4(255.0, 255.0, 255.0, u.f1.w)), 0.0, 255.0));
    const uint cls = TF_TEXA(u.cfg0.z);
    if (cls != 0u)
        t.a = t.a >= 0x40u ? 0x80u : 0u;
    return applyTexa(t, cls);
}

uint4 combineTexture(uint4 v, uint4 t, uint tfx, bool tcc)
{
    uint4 o;
    switch (tfx)
    {
    case 0u:
        o.rgb = min((t.rgb * v.rgb) >> 7, uint3(255u, 255u, 255u));
        o.a = tcc ? min((t.a * v.a) >> 7, 255u) : v.a;
        break;
    case 1u:
        o.rgb = t.rgb;
        o.a = tcc ? t.a : v.a;
        break;
    case 2u:
        o.rgb = min(((t.rgb * v.rgb) >> 7) + v.a, uint3(255u, 255u, 255u));
        o.a = tcc ? min(t.a + v.a, 255u) : v.a;
        break;
    default:
        o.rgb = min(((t.rgb * v.rgb) >> 7) + v.a, uint3(255u, 255u, 255u));
        o.a = tcc ? t.a : v.a;
        break;
    }
    return o;
}

bool alphaTest(uint a, uint w)
{
    if ((w & 1u) == 0u)
        return true;
    const uint aref = (w >> 4) & 0xFFu;
    switch ((w >> 1) & 7u)
    {
    case 0u: return false;
    case 1u: return true;
    case 2u: return a < aref;
    case 3u: return a <= aref;
    case 4u: return a == aref;
    case 5u: return a >= aref;
    case 6u: return a > aref;
    default: return a != aref;
    }
}

int3 pick3(uint sel, int3 cs, int3 cd)
{
    return sel == 0u ? cs : (sel == 1u ? cd : int3(0, 0, 0));
}

struct FSOut
{
    float4 color : SV_Target0;
#if GS_DEPTH_OUT
    float depth : SV_Depth;
#endif
};

FSOut gs_fs(VSOut i)
{
    FSOut o;
    const float4 vc = (u.cfg0.x & F_PERSP_COLOR) != 0u ? i.color / i.tex.z : i.color;
    uint4 c = uint4(clamp((u.cfg0.x & F_EXACT_COLOR) != 0u ? roundAway4(vc) : floor(vc), 0.0, 255.0));

    if ((u.cfg0.x & F_TME) != 0u)
    {
        const uint tf = u.cfg0.z;
        const float texW = float(u.cfg1.w & 0xFFFFu);
        const float texH = float(u.cfg1.w >> 16);
        float2 uv;
        switch (TF_COORD(tf))
        {
        case 0u:
        {
            // STQ: S/Q and T/Q after interpolation, as the GS DDA does.
            const float q = abs(i.tex.z) > 1.0e-8 ? i.tex.z : 1.0;
            const float invQ = 1.0 / q;
            uv = float2(i.tex.x * invQ * texW, i.tex.y * invQ * texH);
            break;
        }
        case 1u:
            // Triangle UV: 1/16 texel units, truncated.
            uv = floor(i.tex.xy) / 16.0;
            break;
        case 2u:
            // Sprite UV: texels, rounded to 1/16.
            uv = floor(i.tex.xy * 16.0 + 0.5) / 16.0;
            break;
        default:
            uv = i.tex.xy;
            break;
        }
        const float2 dx = ddx(uv), dy = ddy(uv);
        if (max(u.f0.x, u.f1.z) > 1.0 && TF_LINEAR(tf) != 0u)
        {
            // Upscaled: pixels near a primitive's edge would filter in
            // texels from outside the range it maps (seams between quads).
            const float2 lo = i.uvBounds.xy + 0.5, hi = i.uvBounds.zw - 0.5;
            uv = select(hi < lo, (i.uvBounds.xy + i.uvBounds.zw) * 0.5, clamp(uv, lo, hi));
        }
        // The mipmap LOD's Q: i.tex.z in every coordinate mode. A
        // replacement has its own mip chain (the sampler's trilinear).
        const uint4 t = u.cfg0.y == 3u     ? sampleReplacement(uv, dx, dy, i.uvBounds)
                        : TF_MXL(tf) != 0u ? sampleMip(uv, i.tex.z)
                                           : sampleTexture(uv);
        c = combineTexture(c, t, TF_TFX(tf), TF_TCC(tf) != 0u);
        if ((u.cfg0.x & F_TINT) != 0u && u.cfg0.y == 3u)
            c.gb = c.gb / 3u;
        if ((u.cfg0.x & F_TINT2) != 0u && u.cfg0.y == 1u)
            c.rg = c.rg / 3u;
    }
    if ((u.cfg0.x & F_NATIVE_TINT) != 0u)
        c.rb = c.rb / 2u;

    if ((u.cfg0.x & F_FGE) != 0u)
    {
        const uint fog = uint(clamp(int(i.tex.w), 0, 255));
        const uint3 fogColor = uint3(u.cfg2.y & 0xFFu, (u.cfg2.y >> 8) & 0xFFu, (u.cfg2.y >> 16) & 0xFFu);
        c.rgb = ((fog * c.rgb) >> 8) + (((255u - fog) * fogColor) >> 8);
    }

    // Alpha test: pass mode 0 draws everything, 1 the passing pixels, 2 the
    // failing ones (TEST.AFAIL, drawn with other write masks).
    const uint passMode = (u.cfg0.w >> 12) & 3u;
    if (passMode != 0u)
    {
        const bool pass = alphaTest(c.a, u.cfg0.w);
        if ((passMode == 1u) != pass)
            discard;
    }

#if GS_DEPTH_OUT
    {
        const float zScale = u.f0.w;
        const float z = min(floor(i.position.z * zScale + 0.5), float(u.cfg3.w));
        o.depth = z / zScale;
    }
#endif

    const uint bits = u.cfg3.z;
    const uint fix = (u.cfg2.w >> 8) & 0xFFu;

#if GS_SLOW
    const uint4 cdRaw = to8(dst.Load(int3(int2(i.position.xy), 0)));
    uint4 cd = (bits & T_16BIT) != 0u ? quantize16(cdRaw) : cdRaw;
    if ((bits & T_CT24) != 0u)
        cd.a = 0x80u;
    if ((bits & T_DATE) != 0u && (bits & T_CT24) == 0u)
    {
        const bool set = (cd.a & 0x80u) != 0u;
        if (set != ((bits & T_DATM) != 0u))
            discard;
    }
    int3 rgb = int3(c.rgb);
    if ((bits & T_ABE) != 0u && !((bits & T_PABE) != 0u && (c.a & 0x80u) == 0u))
    {
        const uint sel = u.cfg2.w;
        const int3 cs = int3(c.rgb);
        const int3 dd = int3(cd.rgb);
        const uint csel = (sel >> 4) & 3u;
        const int ca = csel == 0u ? int(c.a) : (csel == 1u ? int(cd.a) : int(fix));
        rgb = (((pick3(sel & 3u, cs, dd) - pick3((sel >> 2) & 3u, cs, dd)) * ca) >> 7) + pick3((sel >> 6) & 3u, cs, dd);
        rgb = (bits & T_COLCLAMP) != 0u ? clamp(rgb, int3(0, 0, 0), int3(255, 255, 255)) : (rgb & int3(255, 255, 255));
    }
    uint a = c.a;
    if ((bits & T_FBA) != 0u)
        a |= 0x80u;
    uint4 px = uint4(uint3(rgb), a);
    const uint m = u.cfg2.z;
    if (m != 0u)
    {
        const uint4 mask = uint4(m & 0xFFu, (m >> 8) & 0xFFu, (m >> 16) & 0xFFu, m >> 24);
        px = (px & ~mask) | (cdRaw & mask);
    }
    if ((bits & T_KEEP_ALPHA) != 0u)
        px.a = cdRaw.a;
    o.color = float4(px) / 255.0;
#else
    // Hardware blending: the shader makes the source term, the blend unit
    // adds the destination term (see blendSetup() in gs_gpu_backend.cpp).
    const uint csel = (u.cfg2.w >> 4) & 3u;
    const int x = csel == 0u ? int(c.a) : int(fix);
    const int3 cs = int3(c.rgb);
    int3 s;
    switch (u.cfg3.x)
    {
    case 0u: s = cs; break;
    case 1u: s = (cs * x) >> 7; break;
    case 2u: s = cs + ((cs * x) >> 7); break;
    case 3u: s = cs - ((cs * x) >> 7); break;
    case 4u: s = int3(0, 0, 0); break;
    case 5u:
    {
        // COLCLAMP=0 adds that wrap: a term of 128 or more is added as
        // its negative (subtracted by the blend unit).
        const int3 v = ((cs * x) >> 7) & int3(255, 255, 255);
        s = select(v >= int3(128, 128, 128), int3(256, 256, 256) - v, v);
        break;
    }
    default: s = -((cs * x) >> 7); break;
    }
    s = (bits & T_COLCLAMP) != 0u ? clamp(s, int3(0, 0, 0), int3(255, 255, 255)) : (s & int3(255, 255, 255));
    uint a = c.a;
    if ((bits & T_FBA) != 0u)
        a |= 0x80u;
    // Output alpha mode 1: alpha is the blend factor As/128 (stored as
    // As/255 through the alpha blend constant).
    const float outA = u.cfg3.y == 1u ? min(float(c.a) / 128.0, 1.0) : float(a) / 255.0;
    o.color = float4(float3(s) / 255.0, outA);
#endif
    return o;
}

// Full-target passes: a triangle that covers the viewport.
struct BlitOut
{
    float4 position : SV_Position;
};

BlitOut blit_vs(uint vid : SV_VertexID)
{
    const float2 p = float2((vid << 1) & 2u, vid & 2u);
    BlitOut o;
    o.position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

struct BlitUniforms
{
    float4 f; // x/y scale
};
ConstantBuffer<BlitUniforms> bu : register(b0, space3);

// Loads a Z target from local memory: Z (already divided by 2^bits) of each
// GS pixel, in an R32F texture at native resolution.
struct DepthOut
{
    float depth : SV_Depth;
};

Texture2D<float4> src : register(t0, space2);

DepthOut depth_load_fs(BlitOut i)
{
    DepthOut o;
    const uint2 p = uint2(i.position.xy / bu.f.xy);
    const uint2 ss = size2(src);
    o.depth = src.Load(int3(min(p, ss - 1), 0)).r;
    return o;
}

// Writes a Z target to an R32F colour target for read back.
Texture2D<float> depthSrc : register(t0, space2);

float4 depth_store_fs(BlitOut i) : SV_Target0
{
    return float4(depthSrc.Load(int3(int2(i.position.xy), 0)), 0.0, 0.0, 1.0);
}

// Local memory read from a colour target on the GPU (CLUT loads and 8-bit
// textures over a frame buffer that is not written back yet): each output
// texel takes the centre sample of the GS pixel its coordinates texture names
// (x, y in GS pixels; z: byte 0-3 of it, plus 4 for its low nibble or 8 for
// its high one; w: 0 = nothing there), the same
// value a write-back would have put in local memory. bu.f: x/y scale, z/w
// the centre sample's offset.
Texture2D<float4> gatherSrc : register(t0, space2);
Texture2D<uint4> gatherCoords : register(t1, space2);

float4 gather_rgba_fs(BlitOut i) : SV_Target0
{
    const uint4 c = gatherCoords.Load(int3(int2(i.position.xy), 0));
    const int2 p = int2(c.xy) * int2(bu.f.xy) + int2(bu.f.zw);
    return gatherSrc.Load(int3(p, 0));
}

uint gather_index_fs(BlitOut i) : SV_Target0
{
    const uint4 c = gatherCoords.Load(int3(int2(i.position.xy), 0));
    const int2 p = int2(c.xy) * int2(bu.f.xy) + int2(bu.f.zw);
    const float4 v = gatherSrc.Load(int3(p, 0));
    const uint byteIndex = c.z & 3u;
    const float f = byteIndex == 0u ? v.r : (byteIndex == 1u ? v.g : (byteIndex == 2u ? v.b : v.a));
    const uint b = uint(f * 255.0 + 0.5);
    return (c.z & 4u) != 0u ? (b & 15u) : ((c.z & 8u) != 0u ? (b >> 4) : b);
}

// Presentation (PMODE merge of the two read circuits, field mode), at the
// internal resolution.
struct PresentUniforms
{
    int4 src1; // CRT1 origin x, y, width, height (GS pixels)
    int4 src2; // CRT2
    uint4 cfg; // x flags, y ALP, z BGCOLOR, w horizontal scale
    int4 outSize; // output width, height (GS pixels), z vertical scale
};
ConstantBuffer<PresentUniforms> pu : register(b0, space3);

Texture2D<float4> crt1 : register(t0, space2);
Texture2D<float4> crt2 : register(t1, space2);

#define P_VALID1 1u
#define P_VALID2 2u
#define P_16BIT1 4u
#define P_16BIT2 8u
#define P_FIELD 16u
#define P_ODD 32u
#define P_SLBG 64u
#define P_MMOD 128u
#define P_AMOD 256u

float4 readCrt(Texture2D<float4> t, int2 origin, int2 gs, int2 sub, int2 scale, bool is16)
{
    const int2 p = clamp((origin + gs) * scale + sub, int2(0, 0), int2(size2(t)) - 1);
    uint4 c = to8(t.Load(int3(p, 0)));
    if (is16)
    {
        const uint3 c5 = c.rgb >> 3;
        c.rgb = (c5 << 3) | (c5 >> 2);
        c.a = (c.a & 0x80u) != 0u ? 0x80u : 0u;
    }
    return float4(c);
}

float4 present_fs(BlitOut i) : SV_Target0
{
    const int2 scale = int2(int(pu.cfg.w), pu.outSize.z);
    const uint flags = pu.cfg.x;
    const int2 op = int2(i.position.xy);
    int2 gs = op / scale;
    const int2 sub = op - gs * scale;
    if ((flags & P_FIELD) != 0u && pu.outSize.y >= 2)
        gs.y = min(((gs.y >> 1) << 1) + ((flags & P_ODD) != 0u ? 1 : 0), pu.outSize.y - 1);

    const bool v1 = (flags & P_VALID1) != 0u;
    const bool v2 = (flags & P_VALID2) != 0u;
    if (v1 && !v2)
        return float4(readCrt(crt1, pu.src1.xy, gs, sub, scale, (flags & P_16BIT1) != 0u).rgb / 255.0, 1.0);
    if (v2 && !v1)
        return float4(readCrt(crt2, pu.src2.xy, gs, sub, scale, (flags & P_16BIT2) != 0u).rgb / 255.0, 1.0);

    float3 d = float3(pu.cfg.z & 0xFFu, (pu.cfg.z >> 8) & 0xFFu, (pu.cfg.z >> 16) & 0xFFu);
    if ((flags & P_SLBG) == 0u && gs.x < pu.src2.z && gs.y < pu.src2.w)
        d = readCrt(crt2, pu.src2.xy, gs, sub, scale, (flags & P_16BIT2) != 0u).rgb;
    if (gs.x < pu.src1.z && gs.y < pu.src1.w)
    {
        const float4 s = readCrt(crt1, pu.src1.xy, gs, sub, scale, (flags & P_16BIT1) != 0u);
        const float factor = (flags & P_MMOD) != 0u ? float(pu.cfg.y) : min(255.0, s.a * 2.0);
        d = clamp(d + trunc((s.rgb - d) * factor / 255.0), 0.0, 255.0);
    }
    return float4(d / 255.0, 1.0);
}

// Smooth fog in place of a game's fog planes (see fog_fs in the MSL).
struct FogUniforms
{
    float4 colour;
    float4 z;
    float4 n;
    float4 planes[2];
    uint4 cfg;
};

ConstantBuffer<FogUniforms> fu : register(b0, space3);
Texture2D<float> fogDepth : register(t0, space2);

float fogPlanesInFront(float zs)
{
    float count = 0.0;
    for (int k = 0; k < 8; ++k)
    {
        if (float(k) >= fu.n.y)
            break;
        const float pz = fu.planes[k >> 2][k & 3];
        count += (fu.cfg.x != 0u ? pz >= zs : pz > zs) ? 1.0 : 0.0;
    }
    return count;
}

float4 fog_fs(BlitOut i) : SV_Target0
{
    const float zs = fogDepth.Load(int3(int2(i.position.xy), 0)) * fu.z.x;
    float n;
    if (fu.n.z != 0.0)
    {
        const float dz = zs - fu.z.z;
        const float w = dz > 1.0e-6 ? fu.z.y / dz : 1.0e30;
        n = clamp((w - fu.z.w) / fu.n.x + 0.5, 0.0, fu.n.y);
    }
    else
        n = fogPlanesInFront(zs);
    return float4(fu.colour.rgb, 1.0 - pow(fu.colour.w, n));
}

float4 fog_alpha_fs(BlitOut i) : SV_Target0
{
    const float zs = fogDepth.Load(int3(int2(i.position.xy), 0)) * fu.z.x;
    if (fogPlanesInFront(zs) < 1.0)
        discard;
    return float4(0.0, 0.0, 0.0, fu.n.w);
}

// FXAA 3.11 (quality), see fxaa_fs in the MSL.
struct FxaaUniforms
{
    float4 rcp;
};

ConstantBuffer<FxaaUniforms> xu : register(b0, space3);
Texture2D<float4> fxaaSrc : register(t0, space2);
SamplerState fxaaSampler : register(s0, space2);

float fxaaLuma(float2 p)
{
    return dot(fxaaSrc.SampleLevel(fxaaSampler, p, 0.0).rgb, float3(0.299, 0.587, 0.114));
}

float4 fxaa_fs(BlitOut i) : SV_Target0
{
    const float2 rcp = xu.rcp.xy;
    float2 pos = i.position.xy * rcp;
    const float4 rgbM = fxaaSrc.SampleLevel(fxaaSampler, pos, 0.0);
    const float lumaM = dot(rgbM.rgb, float3(0.299, 0.587, 0.114));
    float lumaS = fxaaLuma(pos + float2(0.0, rcp.y));
    const float lumaE = fxaaLuma(pos + float2(rcp.x, 0.0));
    float lumaN = fxaaLuma(pos - float2(0.0, rcp.y));
    const float lumaW = fxaaLuma(pos - float2(rcp.x, 0.0));
    const float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    const float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    const float range = rangeMax - rangeMin;
    if (range < max(0.0625, rangeMax * xu.rcp.w))
        return rgbM;
    const float lumaNW = fxaaLuma(pos + float2(-rcp.x, -rcp.y));
    const float lumaSE = fxaaLuma(pos + float2(rcp.x, rcp.y));
    const float lumaNE = fxaaLuma(pos + float2(rcp.x, -rcp.y));
    const float lumaSW = fxaaLuma(pos + float2(-rcp.x, rcp.y));
    const float lumaNS = lumaN + lumaS;
    const float lumaWE = lumaW + lumaE;
    const float subpixRcpRange = 1.0 / range;
    const float subpixNSWE = lumaNS + lumaWE;
    const float edgeHorz1 = -2.0 * lumaM + lumaNS;
    const float edgeVert1 = -2.0 * lumaM + lumaWE;
    const float lumaNESE = lumaNE + lumaSE;
    const float lumaNWNE = lumaNW + lumaNE;
    const float edgeHorz2 = -2.0 * lumaE + lumaNESE;
    const float edgeVert2 = -2.0 * lumaN + lumaNWNE;
    const float lumaNWSW = lumaNW + lumaSW;
    const float lumaSWSE = lumaSW + lumaSE;
    const float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2);
    const float edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
    const float edgeHorz3 = -2.0 * lumaW + lumaNWSW;
    const float edgeVert3 = -2.0 * lumaS + lumaSWSE;
    const float edgeHorz = abs(edgeHorz3) + edgeHorz4;
    const float edgeVert = abs(edgeVert3) + edgeVert4;
    const float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = rcp.x;
    const bool horzSpan = edgeHorz >= edgeVert;
    const float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan)
    {
        lumaN = lumaW;
        lumaS = lumaE;
    }
    else
        lengthSign = rcp.y;
    const float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    const float gradientN = lumaN - lumaM;
    const float gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM;
    const float lumaSS = lumaS + lumaM;
    const bool pairN = abs(gradientN) >= abs(gradientS);
    const float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN)
        lengthSign = -lengthSign;
    const float subpixC = saturate(abs(subpixB) * subpixRcpRange);
    float2 posB = pos;
    const float2 offNP = horzSpan ? float2(rcp.x, 0.0) : float2(0.0, rcp.y);
    if (!horzSpan)
        posB.x += lengthSign * 0.5;
    else
        posB.y += lengthSign * 0.5;
    float2 posN = posB - offNP;
    float2 posP = posB + offNP;
    const float subpixD = -2.0 * subpixC + 3.0;
    const float subpixE = subpixC * subpixC;
    if (!pairN)
        lumaNN = lumaSS;
    const float gradientScaled = gradient * 0.25;
    const float lumaMM = lumaM - lumaNN * 0.5;
    const float subpixF = subpixD * subpixE;
    const bool lumaMLTZero = lumaMM < 0.0;
    float lumaEndN = fxaaLuma(posN) - lumaNN * 0.5;
    float lumaEndP = fxaaLuma(posP) - lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    const float steps[11] = {1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 12.0, 16.0};
    for (int k = 0; k < 11 && !(doneN && doneP); ++k)
    {
        if (!doneN)
        {
            posN -= offNP * steps[k];
            lumaEndN = fxaaLuma(posN) - lumaNN * 0.5;
            doneN = abs(lumaEndN) >= gradientScaled;
        }
        if (!doneP)
        {
            posP += offNP * steps[k];
            lumaEndP = fxaaLuma(posP) - lumaNN * 0.5;
            doneP = abs(lumaEndP) >= gradientScaled;
        }
    }
    const float dstN = horzSpan ? pos.x - posN.x : pos.y - posN.y;
    const float dstP = horzSpan ? posP.x - pos.x : posP.y - pos.y;
    const bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    const bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    const float spanLengthRcp = 1.0 / (dstP + dstN);
    const bool directionN = dstN < dstP;
    const float dst = min(dstN, dstP);
    const bool goodSpan = directionN ? goodSpanN : goodSpanP;
    const float subpixG = subpixF * subpixF;
    const float pixelOffset = dst * (-spanLengthRcp) + 0.5;
    const float subpixH = subpixG * xu.rcp.z;
    const float pixelOffsetSubpix = max(goodSpan ? pixelOffset : 0.0, subpixH);
    if (!horzSpan)
        pos.x += pixelOffsetSubpix * lengthSign;
    else
        pos.y += pixelOffsetSubpix * lengthSign;
    return float4(fxaaSrc.SampleLevel(fxaaSampler, pos, 0.0).rgb, rgbM.a);
}
