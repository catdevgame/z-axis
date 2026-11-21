#pragma once

// Metal Shading Language source of the SDL_GPU GS backend
// (gs_gpu_backend.cpp), compiled at run time by SDL_CreateGPUShader. Variants
// come from #defines put in front of it:
//   GS_DEPTH_OUT  the fragment shader writes depth (Z rounded to the
//                 integer the GS stores)
//   GS_SLOW       blending, masks and the destination alpha test done in the
//                 shader on a copy of the target (no hardware blending)
// Bindings follow SDL_gpu.h: uniform buffer 0, sampled textures 0-3 with
// samplers 0-3, vertex buffer 0 through [[stage_in]].
//
// The fragment uniforms are laid out as struct GpuFragmentUniforms in
// gs_gpu_backend.cpp; keep both in step.

static const char kGsGpuShaderMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VSIn
{
    float4 pos [[attribute(0)]];   // x, y in GS pixels of the target, z in [0,1]
    float4 color [[attribute(1)]]; // 0-255
    float4 tex [[attribute(2)]];   // s, t, q (or u, v) and fog
    float4 uvBounds [[attribute(3)]]; // the primitive's texel range: u min, v min, u max, v max
};

// Colour and STQ are interpolated linearly on screen, as the GS does (the
// GS-path vertices have w = 1; native draws give the real w for clipping).
struct VSOut
{
    float4 position [[position]];
    float4 color [[center_no_perspective]];
    float4 tex [[center_no_perspective]];
    float4 uvBounds [[flat]];
};

struct VSUniforms
{
    float4 ndc; // 2 / target width, 2 / target height (GS pixels)
};

vertex VSOut gs_vs(VSIn in [[stage_in]], constant VSUniforms &u [[buffer(0)]])
{
    VSOut o;
    o.position = float4(in.pos.x * u.ndc.x - 1.0, 1.0 - in.pos.y * u.ndc.y, in.pos.z, 1.0);
    o.color = in.color;
    o.tex = in.tex;
    o.uvBounds = in.uvBounds;
    return o;
}

// A native draw (runtime/gs/gs_native.h): a mesh's vertices transformed as
// the VU1 program it stands for would (GS window X, Y = x'/w', y'/w'; Z =
// z'/w' clamped at 0; STQ = (s, t, 1) / w'; the colour as given; the FOG
// register's fog), given to the rasterizer in clip space so the GPU clips
// what the GS could not take.
struct NVSIn
{
    float3 pos [[attribute(0)]];
    float2 st [[attribute(1)]];
    uint4 color [[attribute(2)]];
    float4 stBounds [[attribute(3)]];
    int4 normal [[attribute(4)]];  // object space, 32768 = 1.0
};

struct NVSUniforms
{
    float4x4 m;  // rows: [x y z 1] * m (GSNativeDraw::transform)
    float4 view; // XYOFFSET x, y (pixels), 2 / target width, 2 / target height
    float4 misc; // 1 / Z scale, texture width, texture height, fog
    float4 opts; // x: 1 = colour interpolated in perspective (divided by Q in the fragment stage),
                 // y: alpha (< 0: the vertex's), z: normal offset / 32768,
                 // w: S/T transformed by uvm from (s, t, 1) (1) or from the normal / 32768 (2)
    float4 uvm0; // S, T = i.x * uvm0.xy + i.y * uvm0.zw + i.z * uvm1.xy + uvm1.zw
    float4 uvm1;
    float4x4 clip; // rows: [x y z 1] * clip, the VU CLIP flags' volume (GSNativeDraw::clipTransform)
    float4 ext;    // x: GSNativeDraw::dropFlags, y: 1 = X, Y clamped to 0..4095
    // The lights (a lit rigid draw's GSNativeLights, a skinned one's GSNativeSkinning).
    float4 lightDir[3];    // row k: component k of lights 0, 1, 2
    float4 lightColour[4]; // lights 0, 1, 2, the ambient
    float4 light;          // x: 1 = a rigid mesh lit from its normals, y: its colour's scale, z: its alpha
};

// A native draw's vertex output: a fragment's, and the clip distance that
// leaves out a triangle the microprogram would not draw (one of its
// vertices -1, the others 0: nothing of it is >= 0 but an edge).
struct NVSOut
{
    float4 position [[position]];
    float4 color [[center_no_perspective]];
    float4 tex [[center_no_perspective]];
    float4 uvBounds [[flat]];
    float clip [[clip_distance]] [1];
};

// The VU's CLIPw.xyz flags against |max(w, 0)| (gsNativeClipFlags).
static uint clipFlags(float4 c)
{
    const float w = max(c.w, 0.0);
    return (c.x > w ? 1u : 0u) | (c.x < -w ? 2u : 0u) | (c.y > w ? 4u : 0u) | (c.y < -w ? 8u : 0u) |
           (c.z > w ? 16u : 0u) | (c.z < -w ? 32u : 0u);
}

// The common tail of the native vertex shaders: h = the GS window position
// (x', y', z', w'), clipped as GS pixels relative to XYOFFSET; colour, STQ.
static NVSOut nativeOut(float4 h, float4 c, float2 st, float4 b, float q, float drop, float4 view, float4 misc,
                        bool perspectiveColour)
{
    NVSOut o;
    const float w = h.w;
    // Target pixel x = h.x / w - OFX, then to NDC, all times w.
    o.position = float4(h.x * view.z - (view.x * view.z + 1.0) * w,
                        (view.y * view.w + 1.0) * w - h.y * view.w,
                        max(h.z, 0.0) * misc.x,
                        w);
    o.color = perspectiveColour ? c * q : c;
    o.tex = float4(st.x * q, st.y * q, q, misc.w);
    o.uvBounds = b * float4(misc.y, misc.z, misc.y, misc.z);
    o.clip[0] = drop;
    return o;
}

vertex NVSOut gs_native_vs(NVSIn in [[stage_in]], constant NVSUniforms &u [[buffer(0)]])
{
    const float3 p = in.pos + float3(in.normal.xyz) * u.opts.z;
    float4 h = p.x * u.m[0] + p.y * u.m[1] + p.z * u.m[2] + u.m[3];
    if (u.ext.y != 0.0 && h.w > 0.0)
        h.xy = clamp(h.xy / h.w, 0.0, 4095.0) * h.w;
    float drop = 0.0;
    if (u.ext.x != 0.0)
    {
        const float4 c = p.x * u.clip[0] + p.y * u.clip[1] + p.z * u.clip[2] + u.clip[3];
        drop = (clipFlags(c) & uint(u.ext.x)) != 0u ? -1.0 : 0.0;
    }
    float4 c = float4(in.color);
    if (u.light.x != 0.0)
    {
        // A lit rigid mesh (gsNativeLightVertex): FTOI0 of min(scale l, 255), the GS keeps the low byte.
        const float3 n = float3(in.normal.xyz) / 32768.0;
        const float3 d = max((u.lightDir[0].xyz * n.x + u.lightDir[1].xyz * n.y) + u.lightDir[2].xyz * n.z, 0.0);
        const float3 l = ((u.lightColour[3].xyz + u.lightColour[0].xyz * d.x) + u.lightColour[1].xyz * d.y) +
                         u.lightColour[2].xyz * d.z;
        const int3 ic = int3(max(trunc(min(l * u.light.y, 255.0)), -2147483520.0));
        c = float4(float3(ic & int3(255)), u.light.z);
    }
    if (u.opts.y >= 0.0)
        c.a = u.opts.y;
    float2 st = in.st;
    float4 b = in.stBounds;
    if (u.opts.w > 1.5)
    {
        const float3 n = float3(in.normal.xyz) / 32768.0;
        st = n.x * u.uvm0.xy + n.y * u.uvm0.zw + n.z * u.uvm1.xy + u.uvm1.zw;
        b = float4(-1.0e7, -1.0e7, 1.0e7, 1.0e7);
    }
    else if (u.opts.w != 0.0)
    {
        st = st.x * u.uvm0.xy + st.y * u.uvm0.zw + u.uvm1.xy + u.uvm1.zw;
        const float2 b0 = b.x * u.uvm0.xy + b.y * u.uvm0.zw + u.uvm1.xy + u.uvm1.zw;
        const float2 b1 = b.z * u.uvm0.xy + b.w * u.uvm0.zw + u.uvm1.xy + u.uvm1.zw;
        b = float4(min(b0, b1), max(b0, b1));
    }
    return nativeOut(h, c, st, b, 1.0 / h.w, drop, u.view, u.misc, u.opts.x != 0.0);
}

// A skinned native draw (GSNativeSkinning; AI's VU1 0x08 / 0x0C programs):
// each vertex moved by up to four bones of the draw's palette, lit by three
// directional lights and an ambient or with its own colour, then as
// gs_native_vs with STQ = (s, t, 1) (affine: GSNativeDraw::affineUV).
struct SVSIn
{
    float3 pos [[attribute(0)]];   // with the bones' addresses in the low mantissa bits, as VU1 gets it
    float2 st [[attribute(1)]];
    uint4 color [[attribute(2)]];
    float4 stBounds [[attribute(3)]];
    int4 normal [[attribute(4)]];  // 32768 = 1.0 (weights in the low bits, as VU1 reads it)
    uint4 bone [[attribute(5)]];   // GSNativeSkinWeights: palette rows
    uint4 weight [[attribute(6)]]; // sixteenths
};

struct SVSUniforms
{
    NVSUniforms n;         // with the lights
    float4 skin;           // x: 1 = lit, y: the colour's cap (255 as VU1, 128 the Xbox lighting), z: 1 = alpha lit too
    float4 palette[128];   // VU1 0..127
};

vertex NVSOut gs_native_skin_vs(SVSIn in [[stage_in]], constant SVSUniforms &u [[buffer(0)]])
{
    float3 p = float3(0.0);
    for (int i = 0; i < 4; ++i)
    {
        const uint a = min(in.bone[i], 124u);
        const float k = float(in.weight[i]) / 16.0;
        p += (((u.palette[a + 3].xyz + u.palette[a].xyz * in.pos.x) + u.palette[a + 1].xyz * in.pos.y) +
              u.palette[a + 2].xyz * in.pos.z) * k;
    }
    float4 c = float4(in.color);
    if (u.skin.x != 0.0)
    {
        const uint a = min(in.bone.x, 124u);
        const float3 n = float3(in.normal.xyz) / 32768.0;
        const float3 nn = (u.palette[a].xyz * n.x + u.palette[a + 1].xyz * n.y) + u.palette[a + 2].xyz * n.z;
        const float3 d = max((u.n.lightDir[0].xyz * nn.x + u.n.lightDir[1].xyz * nn.y) + u.n.lightDir[2].xyz * nn.z, 0.0);
        const float3 l = ((u.n.lightColour[3].xyz + u.n.lightColour[0].xyz * d.x) + u.n.lightColour[1].xyz * d.y) +
                         u.n.lightColour[2].xyz * d.z;
        // FTOI0 of min(128 l, 255); the GS keeps the low byte.
        const int3 ic = int3(max(trunc(min(l * 128.0, u.skin.y)), -2147483520.0));
        c = float4(float3(ic & int3(255)), 128.0);
        if (u.skin.z != 0.0)
        {
            // DM2's lit program: the alpha from the colours' w, not capped (FTOI0, the low byte).
            const float la = ((u.n.lightColour[3].w + u.n.lightColour[0].w * d.x) + u.n.lightColour[1].w * d.y) +
                             u.n.lightColour[2].w * d.z;
            c.w = float(int(clamp(trunc(la * 128.0), -2147483520.0, 2147483520.0)) & 255);
        }
    }
    const float4 h = p.x * u.n.m[0] + p.y * u.n.m[1] + p.z * u.n.m[2] + u.n.m[3];
    float drop = 0.0;
    if (u.n.ext.x != 0.0)
    {
        const float4 cc = p.x * u.n.clip[0] + p.y * u.n.clip[1] + p.z * u.n.clip[2] + u.n.clip[3];
        drop = (clipFlags(cc) & uint(u.n.ext.x)) != 0u ? -1.0 : 0.0;
    }
    return nativeOut(h, c, in.st, in.stBounds, u.n.ext.z != 0.0 ? 1.0 : 1.0 / h.w, drop, u.n.view, u.n.misc,
                     u.n.opts.x != 0.0);
}

struct FSUniforms
{
    uint4 cfg0;  // x flags, y texture kind (0 RGBA, 1 indexed, 2 render target, 3 replacement), z texture function bits, w alpha test bits
    uint4 cfg1;  // x wrap modes, y minU | maxU << 16, z minV | maxV << 16, w texW | texH << 16
    uint4 cfg2;  // x ta0 | ta1 << 8 | aem << 16, y fog color, z FBMSK, w blend A | B << 2 | C << 4 | D << 6 | FIX << 8
    uint4 cfg3;  // x source term, y output alpha mode, z target bits, w max Z
    float4 f0;   // x scale (horizontal), y/z texture origin in the source target (GS pixels), w Z scale (2^bits)
    float4 f1;   // x/y source target size (GS pixels), z vertical scale, w mipmap K (replacement: alpha for 1.0)
    float4 f2;   // replacement: the GS texels its picture covers, x/y origin, z/w size (z 0: the whole texture)
};

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

struct FSOut
{
    float4 color [[color(0)]];
#if GS_DEPTH_OUT
    float depth [[depth(any)]];
#endif
};

static uint4 to8(float4 c)
{
    return uint4(clamp(round(c * 255.0), 0.0, 255.0));
}

static uint4 quantize16(uint4 c)
{
    return uint4(c.rgb & 0xF8u, (c.a & 0x80u) != 0u ? 0x80u : 0u);
}

static uint4 applyTexa(uint4 c, uint cls, constant FSUniforms &u)
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

static int wrapCoord(int c, int size, uint mode, int lo, int hi)
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
static uint4 fetchTexel(int2 t, uint lod, constant FSUniforms &u,
                        texture2d<float> tex, texture2d<uint> idx, texture2d<float> pal)
{
    const int texW = max(int(u.cfg1.w & 0xFFFFu) >> lod, 1);
    const int texH = max(int(u.cfg1.w >> 16) >> lod, 1);
    const int x = wrapCoord(t.x, texW, u.cfg1.x & 3u, int(u.cfg1.y & 0xFFFFu) >> lod, int(u.cfg1.y >> 16) >> lod);
    const int y = wrapCoord(t.y, texH, (u.cfg1.x >> 2) & 3u, int(u.cfg1.z & 0xFFFFu) >> lod, int(u.cfg1.z >> 16) >> lod);
    uint4 c;
    if (u.cfg0.y == 1u)
    {
        const uint2 q = uint2(clamp(x, 0, int(idx.get_width(lod)) - 1), clamp(y, 0, int(idx.get_height(lod)) - 1));
        c = to8(pal.read(uint2(idx.read(q, lod).r, 0u)));
    }
    else
    {
        const uint2 p = uint2(clamp(x, 0, int(tex.get_width(lod)) - 1), clamp(y, 0, int(tex.get_height(lod)) - 1));
        c = to8(tex.read(p, lod));
    }
    return applyTexa(c, TF_TEXA(u.cfg0.z), u);
}

// A texel of a render target used as a texture, at the internal resolution.
static uint4 fetchTarget(int2 p, constant FSUniforms &u, texture2d<float> tex)
{
    p = clamp(p, int2(0), int2(tex.get_width(), tex.get_height()) - 1);
    uint4 c = to8(tex.read(uint2(p)));
    if (TF_SRC16(u.cfg0.z) != 0u)
        c = quantize16(c);
    return applyTexa(c, TF_TEXA(u.cfg0.z), u);
}

static uint lerp8(uint c00, uint c10, uint c01, uint c11, float fx, float fy)
{
    const float top = float(c00) + (float(c10) - float(c00)) * fx;
    const float bottom = float(c01) + (float(c11) - float(c01)) * fx;
    return uint(clamp(round(top + (bottom - top) * fy), 0.0, 255.0));
}

static uint4 lerp4(uint4 c00, uint4 c10, uint4 c01, uint4 c11, float fx, float fy)
{
    return uint4(lerp8(c00.r, c10.r, c01.r, c11.r, fx, fy),
                 lerp8(c00.g, c10.g, c01.g, c11.g, fx, fy),
                 lerp8(c00.b, c10.b, c01.b, c11.b, fx, fy),
                 lerp8(c00.a, c10.a, c01.a, c11.a, fx, fy));
}

static uint4 sampleLevel(float2 uv, uint lod, bool linear, constant FSUniforms &u,
                         texture2d<float> tex, texture2d<uint> idx, texture2d<float> pal)
{
    if (!linear)
        return fetchTexel(int2(uv), lod, u, tex, idx, pal);
    const float2 s = uv - 0.5;
    const int2 p0 = int2(floor(s));
    const float2 f = s - float2(p0);
    return lerp4(fetchTexel(p0, lod, u, tex, idx, pal), fetchTexel(p0 + int2(1, 0), lod, u, tex, idx, pal),
                 fetchTexel(p0 + int2(0, 1), lod, u, tex, idx, pal), fetchTexel(p0 + int2(1, 1), lod, u, tex, idx, pal), f.x, f.y);
}

// GS mipmapping (gsMipSelect): LOD = (log2(1/|Q|) << L) + K, or K (LCM).
// LOD < 0 is MMAG on level 0; otherwise MMIN, with NEAREST_MIPMAP rounding
// LOD to a level and LINEAR_MIPMAP blending floor(LOD) and the next level.
static uint4 sampleMip(float2 uv, float q, constant FSUniforms &u,
                       texture2d<float> tex, texture2d<uint> idx, texture2d<float> pal)
{
    const uint tf = u.cfg0.z;
    const uint mxl = TF_MXL(tf);
    float lod = u.f1.w;
    if (TF_LCM(tf) == 0u)
        lod += -log2(max(abs(q), 1.0e-30)) * float(1u << TF_MIPL(tf));
    uint level = 0u, level2 = 0u;
    float frac = 0.0;
    bool linear;
    if (!(lod >= 0.0))
        linear = TF_MAGLIN(tf) != 0u;
    else
    {
        linear = TF_MINLIN(tf) != 0u;
        if (TF_MIPLIN(tf) == 0u)
            level = level2 = uint(min(floor(lod + 0.5), float(mxl)));
        else if (lod >= float(mxl))
            level = level2 = mxl;
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
    const uint4 c0 = sampleLevel(uv / float(1u << level), level, linear, u, tex, idx, pal);
    if (level2 == level || !(frac > 0.0))
        return c0;
    const uint4 c1 = sampleLevel(uv / float(1u << level2), level2, linear, u, tex, idx, pal);
    return uint4(clamp(round(float4(c0) + (float4(c1) - float4(c0)) * frac), 0.0, 255.0));
}

static uint4 sampleTexture(float2 uv, constant FSUniforms &u,
                           texture2d<float> tex, texture2d<uint> idx, texture2d<float> pal)
{
    const bool linear = TF_LINEAR(u.cfg0.z) != 0u;
    if (u.cfg0.y == 2u)
    {
        // Render target: sample it at its own resolution.
        const float2 sp = (uv + u.f0.yz) * float2(u.f0.x, u.f1.z);
        if (!linear)
            return fetchTarget(int2(floor(sp)), u, tex);
        const float2 s = sp - 0.5;
        const int2 p0 = int2(floor(s));
        const float2 f = s - float2(p0);
        return lerp4(fetchTarget(p0, u, tex), fetchTarget(p0 + int2(1, 0), u, tex),
                     fetchTarget(p0 + int2(0, 1), u, tex), fetchTarget(p0 + int2(1, 1), u, tex), f.x, f.y);
    }
    return sampleLevel(uv, 0u, linear, u, tex, idx, pal);
}

// A replacement picture (texture kind 3: runtime/ps2_texture_replace.h) for
// an indexed texture: any size, RGBA with mips, sampled trilinear at the
// same place in the texture (u / TW, v / TH) instead of the CLUT. Alpha
// 1.0 is the PS2 palette's largest alpha (f1.w: 0x80, or 0x7F as the game
// has it). dx/dy: the derivatives of uv for the mip level. Also a native
// draw's PS2 texture as a mipmapped copy (runtime/gs/gs_texture_mips.h;
// f1.w 255: its alpha bytes as they are, TEXA applied already).
static uint4 sampleReplacement(float2 uv, float2 dx, float2 dy, float4 bounds, constant FSUniforms &u,
                               texture2d<float> tex, sampler s)
{
    const float2 texSize = float2(float(u.cfg1.w & 0xFFFFu), float(u.cfg1.w >> 16));
    const float2 size = float2(float(tex.get_width()), float(tex.get_height()));
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
    uv = select(clamp(uv, lo, hi), (bounds.xy + bounds.zw) * 0.5, hi < lo);
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
        c = tex.read(uint2(clamp(floor((uv - origin) * size / area), float2(0.0), size - 1.0)), 0);
    }
    else
        c = tex.sample(s, (uv - origin) / area, gradient2d(dx / area, dy / area));
    uint4 t = uint4(clamp(round(c * float4(255.0, 255.0, 255.0, u.f1.w)), 0.0, 255.0));
    const uint cls = TF_TEXA(u.cfg0.z);
    if (cls != 0u)
        t.a = t.a >= 0x40u ? 0x80u : 0u; // 16-bit CLUT: the alpha bit picks TA0 / TA1
    return applyTexa(t, cls, u);
}

static uint4 combineTexture(uint4 v, uint4 t, uint tfx, bool tcc)
{
    uint4 o;
    switch (tfx)
    {
    case 0u:
        o.rgb = min((t.rgb * v.rgb) >> 7, uint3(255u));
        o.a = tcc ? min((t.a * v.a) >> 7, 255u) : v.a;
        break;
    case 1u:
        o.rgb = t.rgb;
        o.a = tcc ? t.a : v.a;
        break;
    case 2u:
        o.rgb = min(((t.rgb * v.rgb) >> 7) + v.a, uint3(255u));
        o.a = tcc ? min(t.a + v.a, 255u) : v.a;
        break;
    default:
        o.rgb = min(((t.rgb * v.rgb) >> 7) + v.a, uint3(255u));
        o.a = tcc ? t.a : v.a;
        break;
    }
    return o;
}

static bool alphaTest(uint a, uint w)
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

static int3 pick3(uint sel, int3 cs, int3 cd)
{
    return sel == 0u ? cs : (sel == 1u ? cd : int3(0));
}

fragment FSOut gs_fs(VSOut in [[stage_in]], constant FSUniforms &u [[buffer(0)]],
                     texture2d<float> tex [[texture(0)]], texture2d<uint> idx [[texture(1)]],
                     texture2d<float> pal [[texture(2)]], texture2d<float> dst [[texture(3)]],
                     sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]],
                     sampler s2 [[sampler(2)]], sampler s3 [[sampler(3)]])
{
    FSOut o;
    const float4 vc = (u.cfg0.x & F_PERSP_COLOR) != 0u ? in.color / in.tex.z : in.color;
    uint4 c = uint4(clamp((u.cfg0.x & F_EXACT_COLOR) != 0u ? round(vc) : floor(vc), 0.0, 255.0));

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
            const float q = abs(in.tex.z) > 1.0e-8 ? in.tex.z : 1.0;
            const float invQ = 1.0 / q;
            uv = float2(in.tex.x * invQ * texW, in.tex.y * invQ * texH);
            break;
        }
        case 1u:
            // Triangle UV: 1/16 texel units, truncated.
            uv = floor(in.tex.xy) / 16.0;
            break;
        case 2u:
            // Sprite UV: texels, rounded to 1/16.
            uv = floor(in.tex.xy * 16.0 + 0.5) / 16.0;
            break;
        default:
            uv = in.tex.xy;
            break;
        }
        const float2 dx = dfdx(uv), dy = dfdy(uv);
        if (max(u.f0.x, u.f1.z) > 1.0 && TF_LINEAR(tf) != 0u)
        {
            // Upscaled: pixels near a primitive's edge would filter in
            // texels from outside the range it maps (seams between quads).
            const float2 lo = in.uvBounds.xy + 0.5, hi = in.uvBounds.zw - 0.5;
            uv = select(clamp(uv, lo, hi), (in.uvBounds.xy + in.uvBounds.zw) * 0.5, hi < lo);
        }
        // The mipmap LOD's Q: in.tex.z in every coordinate mode. A
        // replacement has its own mip chain (the sampler's trilinear).
        const uint4 t = u.cfg0.y == 3u     ? sampleReplacement(uv, dx, dy, in.uvBounds, u, tex, s0)
                        : TF_MXL(tf) != 0u ? sampleMip(uv, in.tex.z, u, tex, idx, pal)
                                           : sampleTexture(uv, u, tex, idx, pal);
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
        const uint fog = uint(clamp(int(in.tex.w), 0, 255));
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
            discard_fragment();
    }

#if GS_DEPTH_OUT
    {
        const float zScale = u.f0.w;
        const float z = min(floor(in.position.z * zScale + 0.5), float(u.cfg3.w));
        o.depth = z / zScale;
    }
#endif

    const uint bits = u.cfg3.z;
    const uint fix = (u.cfg2.w >> 8) & 0xFFu;

#if GS_SLOW
    const uint4 cdRaw = to8(dst.read(uint2(in.position.xy)));
    uint4 cd = (bits & T_16BIT) != 0u ? quantize16(cdRaw) : cdRaw;
    if ((bits & T_CT24) != 0u)
        cd.a = 0x80u;
    if ((bits & T_DATE) != 0u && (bits & T_CT24) == 0u)
    {
        const bool set = (cd.a & 0x80u) != 0u;
        if (set != ((bits & T_DATM) != 0u))
            discard_fragment();
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
        rgb = (bits & T_COLCLAMP) != 0u ? clamp(rgb, int3(0), int3(255)) : (rgb & int3(255));
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
    case 4u: s = int3(0); break;
    case 5u:
    {
        // COLCLAMP=0 adds that wrap: a term of 128 or more is added as
        // its negative (subtracted by the blend unit).
        const int3 v = ((cs * x) >> 7) & int3(255);
        s = select(v, int3(256) - v, v >= int3(128));
        break;
    }
    default: s = -((cs * x) >> 7); break;
    }
    s = (bits & T_COLCLAMP) != 0u ? clamp(s, int3(0), int3(255)) : (s & int3(255));
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
    float4 position [[position]];
};

vertex BlitOut blit_vs(uint vid [[vertex_id]])
{
    const float2 p = float2((vid << 1) & 2u, vid & 2u);
    BlitOut o;
    o.position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

struct BlitUniforms
{
    float4 f;  // x/y scale
};

// Loads a Z target from local memory: Z (already divided by 2^bits) of each
// GS pixel, in an R32F texture at native resolution.
struct DepthOut
{
    float depth [[depth(any)]];
};

fragment DepthOut depth_load_fs(BlitOut in [[stage_in]], constant BlitUniforms &u [[buffer(0)]],
                                texture2d<float> src [[texture(0)]], sampler s0 [[sampler(0)]])
{
    DepthOut o;
    const uint2 p = uint2(in.position.xy / u.f.xy);
    o.depth = src.read(min(p, uint2(src.get_width(), src.get_height()) - 1)).r;
    return o;
}

// Writes a Z target to an R32F colour target for read back.
fragment float4 depth_store_fs(BlitOut in [[stage_in]], depth2d<float> src [[texture(0)]],
                               sampler s0 [[sampler(0)]])
{
    return float4(src.read(uint2(in.position.xy)), 0.0, 0.0, 1.0);
}

// Local memory read from a colour target on the GPU (CLUT loads and 8-bit
// textures over a frame buffer that is not written back yet): each output
// texel takes the centre sample of the GS pixel its coordinates texture names
// (x, y in GS pixels; z: byte 0-3 of it, plus 4 for its low nibble or 8 for
// its high one; w: 0 = nothing there), the same
// value a write-back would have put in local memory. u.f: x/y scale, z/w
// the centre sample's offset.
fragment float4 gather_rgba_fs(BlitOut in [[stage_in]], constant BlitUniforms &u [[buffer(0)]],
                               texture2d<float> src [[texture(0)]], texture2d<uint> coords [[texture(1)]],
                               sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]])
{
    const uint4 c = coords.read(uint2(in.position.xy));
    const uint2 p = c.xy * uint2(u.f.xy) + uint2(u.f.zw);
    return src.read(p);
}

fragment uint gather_index_fs(BlitOut in [[stage_in]], constant BlitUniforms &u [[buffer(0)]],
                              texture2d<float> src [[texture(0)]], texture2d<uint> coords [[texture(1)]],
                              sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]])
{
    const uint4 c = coords.read(uint2(in.position.xy));
    const uint2 p = c.xy * uint2(u.f.xy) + uint2(u.f.zw);
    const float4 v = src.read(p);
    const uint byteIndex = c.z & 3u;
    const float f = byteIndex == 0u ? v.r : (byteIndex == 1u ? v.g : (byteIndex == 2u ? v.b : v.a));
    const uint b = uint(f * 255.0 + 0.5);
    return (c.z & 4u) != 0u ? (b & 15u) : ((c.z & 8u) != 0u ? (b >> 4) : b);
}

// Presentation (PMODE merge of the two read circuits, field mode), at the
// internal resolution.
struct PresentUniforms
{
    int4 src1;   // CRT1 origin x, y, width, height (GS pixels)
    int4 src2;   // CRT2
    uint4 cfg;   // x flags, y ALP, z BGCOLOR, w horizontal scale
    int4 out;    // output width, height (GS pixels), z vertical scale
};

#define P_VALID1 1u
#define P_VALID2 2u
#define P_16BIT1 4u
#define P_16BIT2 8u
#define P_FIELD 16u
#define P_ODD 32u
#define P_SLBG 64u
#define P_MMOD 128u
#define P_AMOD 256u

static float4 readCrt(texture2d<float> t, int2 origin, int2 gs, int2 sub, int2 scale, bool is16)
{
    const int2 p = clamp((origin + gs) * scale + sub, int2(0), int2(t.get_width(), t.get_height()) - 1);
    uint4 c = to8(t.read(uint2(p)));
    if (is16)
    {
        const uint3 c5 = c.rgb >> 3;
        c.rgb = (c5 << 3) | (c5 >> 2);
        c.a = (c.a & 0x80u) != 0u ? 0x80u : 0u;
    }
    return float4(c);
}

fragment float4 present_fs(BlitOut in [[stage_in]], constant PresentUniforms &u [[buffer(0)]],
                           texture2d<float> t1 [[texture(0)]], texture2d<float> t2 [[texture(1)]],
                           sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]])
{
    const int2 scale = int2(int(u.cfg.w), u.out.z);
    const uint flags = u.cfg.x;
    const int2 op = int2(in.position.xy);
    int2 gs = op / scale;
    const int2 sub = op - gs * scale;
    if ((flags & P_FIELD) != 0u && u.out.y >= 2)
        gs.y = min(((gs.y >> 1) << 1) + ((flags & P_ODD) != 0u ? 1 : 0), u.out.y - 1);

    const bool v1 = (flags & P_VALID1) != 0u;
    const bool v2 = (flags & P_VALID2) != 0u;
    if (v1 && !v2)
        return float4(readCrt(t1, u.src1.xy, gs, sub, scale, (flags & P_16BIT1) != 0u).rgb / 255.0, 1.0);
    if (v2 && !v1)
        return float4(readCrt(t2, u.src2.xy, gs, sub, scale, (flags & P_16BIT2) != 0u).rgb / 255.0, 1.0);

    float3 d = float3(u.cfg.z & 0xFFu, (u.cfg.z >> 8) & 0xFFu, (u.cfg.z >> 16) & 0xFFu);
    if ((flags & P_SLBG) == 0u && gs.x < u.src2.z && gs.y < u.src2.w)
        d = readCrt(t2, u.src2.xy, gs, sub, scale, (flags & P_16BIT2) != 0u).rgb;
    if (gs.x < u.src1.z && gs.y < u.src1.w)
    {
        const float4 s = readCrt(t1, u.src1.xy, gs, sub, scale, (flags & P_16BIT1) != 0u);
        const float factor = (flags & P_MMOD) != 0u ? float(u.cfg.y) : min(255.0, s.a * 2.0);
        d = clamp(d + trunc((s.rgb - d) * factor / 255.0), 0.0, 255.0);
    }
    return float4(d / 255.0, 1.0);
}
// Smooth fog in place of a game's fog planes (GSFogPlanes): per pixel from
// the depth buffer. n planes of colour F and alpha As over a pixel give F +
// (S - F) k^n, k = 1 - As/128; the PS2 counts the planes in front of the
// pixel (whole steps), here n follows its view distance w = A / (Z - B)
// smoothly, through the middle of each step. Drawn with blending: colour
// F, alpha 1 - k^n (source alpha over the destination).
struct FogUniforms
{
    float4 colour; // rgb 0..1, w: k
    float4 z;      // x: 2^bits of the Z buffer, y: A, z: B, w: nearest plane's distance
    float4 n;      // x: step, y: planes, z: 1 = smooth (0: whole steps), w: alpha written (0..1)
    float4 planes[2]; // the planes' GS Z, nearest first (whole steps and the alpha's coverage)
    uint4 cfg;     // x: 1 = the planes pass where Z >= (GEQUAL), else Z > (GREATER)
};

static float fogPlanesInFront(float zs, constant FogUniforms &u)
{
    float count = 0.0;
    for (int k = 0; k < 8; ++k)
    {
        if (float(k) >= u.n.y)
            break;
        const float pz = u.planes[k >> 2][k & 3];
        count += (u.cfg.x != 0u ? pz >= zs : pz > zs) ? 1.0 : 0.0;
    }
    return count;
}

fragment float4 fog_fs(BlitOut in [[stage_in]], constant FogUniforms &u [[buffer(0)]],
                       depth2d<float> depth [[texture(0)]], sampler s0 [[sampler(0)]])
{
    const float zs = depth.read(uint2(in.position.xy)) * u.z.x;
    float n;
    if (u.n.z != 0.0)
    {
        const float dz = zs - u.z.z;
        const float w = dz > 1.0e-6 ? u.z.y / dz : 1.0e30;
        n = clamp((w - u.z.w) / u.n.x + 0.5, 0.0, u.n.y);
    }
    else
        n = fogPlanesInFront(zs, u);
    return float4(u.colour.rgb, 1.0 - pow(u.colour.w, n));
}

// The planes' alpha where at least one of them covers the pixel (alpha only).
fragment float4 fog_alpha_fs(BlitOut in [[stage_in]], constant FogUniforms &u [[buffer(0)]],
                             depth2d<float> depth [[texture(0)]], sampler s0 [[sampler(0)]])
{
    const float zs = depth.read(uint2(in.position.xy)) * u.z.x;
    if (fogPlanesInFront(zs, u) < 1.0)
        discard_fragment();
    return float4(0.0, 0.0, 0.0, u.n.w);
}

// FXAA 3.11 (quality), on a copy of the target: edges found by luma
// contrast, blended along them. u.rcp: 1 / the copy's size.
struct FxaaUniforms
{
    float4 rcp; // x, y: 1 / width, 1 / height; z: subpixel amount, w: edge threshold
};

static float fxaaLuma(texture2d<float> t, sampler s, float2 p)
{
    return dot(t.sample(s, p, level(0.0)).rgb, float3(0.299, 0.587, 0.114));
}

fragment float4 fxaa_fs(BlitOut in [[stage_in]], constant FxaaUniforms &u [[buffer(0)]],
                        texture2d<float> tex [[texture(0)]], sampler s0 [[sampler(0)]])
{
    const float2 rcp = u.rcp.xy;
    float2 pos = in.position.xy * rcp;
    const float4 rgbM = tex.sample(s0, pos, level(0.0));
    const float lumaM = dot(rgbM.rgb, float3(0.299, 0.587, 0.114));
    float lumaS = fxaaLuma(tex, s0, pos + float2(0.0, rcp.y));
    const float lumaE = fxaaLuma(tex, s0, pos + float2(rcp.x, 0.0));
    float lumaN = fxaaLuma(tex, s0, pos - float2(0.0, rcp.y));
    const float lumaW = fxaaLuma(tex, s0, pos - float2(rcp.x, 0.0));
    const float rangeMax = max(max(lumaN, lumaW), max(lumaE, max(lumaS, lumaM)));
    const float rangeMin = min(min(lumaN, lumaW), min(lumaE, min(lumaS, lumaM)));
    const float range = rangeMax - rangeMin;
    if (range < max(0.0625, rangeMax * u.rcp.w))
        return rgbM;
    const float lumaNW = fxaaLuma(tex, s0, pos + float2(-rcp.x, -rcp.y));
    const float lumaSE = fxaaLuma(tex, s0, pos + float2(rcp.x, rcp.y));
    const float lumaNE = fxaaLuma(tex, s0, pos + float2(rcp.x, -rcp.y));
    const float lumaSW = fxaaLuma(tex, s0, pos + float2(-rcp.x, rcp.y));
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
    float lumaEndN = fxaaLuma(tex, s0, posN) - lumaNN * 0.5;
    float lumaEndP = fxaaLuma(tex, s0, posP) - lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    const float steps[11] = {1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0, 12.0, 16.0};
    for (int i = 0; i < 11 && !(doneN && doneP); ++i)
    {
        if (!doneN)
        {
            posN -= offNP * steps[i];
            lumaEndN = fxaaLuma(tex, s0, posN) - lumaNN * 0.5;
            doneN = abs(lumaEndN) >= gradientScaled;
        }
        if (!doneP)
        {
            posP += offNP * steps[i];
            lumaEndP = fxaaLuma(tex, s0, posP) - lumaNN * 0.5;
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
    const float subpixH = subpixG * u.rcp.z;
    const float pixelOffsetSubpix = max(goodSpan ? pixelOffset : 0.0, subpixH);
    if (!horzSpan)
        pos.x += pixelOffsetSubpix * lengthSign;
    else
        pos.y += pixelOffsetSubpix * lengthSign;
    return float4(tex.sample(s0, pos, level(0.0)).rgb, rgbM.a);
}
)MSL";
