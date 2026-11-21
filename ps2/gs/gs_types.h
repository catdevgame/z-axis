#pragma once

#include <cstddef>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

enum GSPrimType : uint8_t
{
    GS_PRIM_POINT = 0,
    GS_PRIM_LINE = 1,
    GS_PRIM_LINESTRIP = 2,
    GS_PRIM_TRIANGLE = 3,
    GS_PRIM_TRISTRIP = 4,
    GS_PRIM_TRIFAN = 5,
    GS_PRIM_SPRITE = 6,
};

enum GSPsm : uint8_t
{
    GS_PSM_CT32 = 0,
    GS_PSM_CT24 = 1,
    GS_PSM_CT16 = 2,
    GS_PSM_CT16S = 10,
    GS_PSM_T8 = 19,
    GS_PSM_T4 = 20,
    GS_PSM_T8H = 27,
    GS_PSM_T4HL = 36,
    GS_PSM_T4HH = 44,
    GS_PSM_Z32 = 48,
    GS_PSM_Z24 = 49,
    GS_PSM_Z16 = 50,
    GS_PSM_Z16S = 58,
};

enum GSGifFormat : uint8_t
{
    GIF_FMT_PACKED = 0,
    GIF_FMT_REGLIST = 1,
    GIF_FMT_IMAGE = 2,
    GIF_FMT_DISABLED = 3,
};

enum GSRegId : uint8_t
{
    GS_REG_PRIM = 0x00,
    GS_REG_RGBAQ = 0x01,
    GS_REG_ST = 0x02,
    GS_REG_UV = 0x03,
    GS_REG_XYZF2 = 0x04,
    GS_REG_XYZ2 = 0x05,
    GS_REG_TEX0_1 = 0x06,
    GS_REG_TEX0_2 = 0x07,
    GS_REG_CLAMP_1 = 0x08,
    GS_REG_CLAMP_2 = 0x09,
    GS_REG_FOG = 0x0A,
    GS_REG_XYZF3 = 0x0C,
    GS_REG_XYZ3 = 0x0D,
    GS_REG_AD = 0x0F,
    GS_REG_TEX1_1 = 0x14,
    GS_REG_TEX1_2 = 0x15,
    GS_REG_TEX2_1 = 0x16,
    GS_REG_TEX2_2 = 0x17,
    GS_REG_XYOFFSET_1 = 0x18,
    GS_REG_XYOFFSET_2 = 0x19,
    GS_REG_PRMODECONT = 0x1A,
    GS_REG_PRMODE = 0x1B,
    GS_REG_TEXCLUT = 0x1C,
    GS_REG_SCANMSK = 0x22,
    GS_REG_MIPTBP1_1 = 0x34,
    GS_REG_MIPTBP1_2 = 0x35,
    GS_REG_MIPTBP2_1 = 0x36,
    GS_REG_MIPTBP2_2 = 0x37,
    GS_REG_TEXA = 0x3B,
    GS_REG_FOGCOL = 0x3D,
    GS_REG_TEXFLUSH = 0x3F,
    GS_REG_SCISSOR_1 = 0x40,
    GS_REG_SCISSOR_2 = 0x41,
    GS_REG_ALPHA_1 = 0x42,
    GS_REG_ALPHA_2 = 0x43,
    GS_REG_DIMX = 0x44,
    GS_REG_DTHE = 0x45,
    GS_REG_COLCLAMP = 0x46,
    GS_REG_TEST_1 = 0x47,
    GS_REG_TEST_2 = 0x48,
    GS_REG_PABE = 0x49,
    GS_REG_FBA_1 = 0x4A,
    GS_REG_FBA_2 = 0x4B,
    GS_REG_FRAME_1 = 0x4C,
    GS_REG_FRAME_2 = 0x4D,
    GS_REG_ZBUF_1 = 0x4E,
    GS_REG_ZBUF_2 = 0x4F,
    GS_REG_BITBLTBUF = 0x50,
    GS_REG_TRXPOS = 0x51,
    GS_REG_TRXREG = 0x52,
    GS_REG_TRXDIR = 0x53,
    GS_REG_HWREG = 0x54,
    GS_REG_SIGNAL = 0x60,
    GS_REG_FINISH = 0x61,
    GS_REG_LABEL = 0x62,
};

struct GSVertex
{
    float x = 0.0f;
    float y = 0.0f;
    double z = 0.0;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;
    float q = 1.0f;
    float s = 0.0f;
    float t = 0.0f;
    uint16_t u = 0;
    uint16_t v = 0;
    uint8_t fog = 0;
};

struct GSFrameReg
{
    uint32_t fbp = 0;
    uint32_t fbw = 0;
    uint8_t psm = 0;
    uint32_t fbmsk = 0;
};

struct GSZbufReg
{
    uint32_t zbp = 0;
    uint8_t psm = 0;
    bool zmask = false;
};

struct GSScissorReg
{
    uint16_t x0 = 0;
    uint16_t x1 = 0;
    uint16_t y0 = 0;
    uint16_t y1 = 0;
};

struct GSTex0Reg
{
    uint32_t tbp0 = 0;
    uint8_t tbw = 0;
    uint8_t psm = 0;
    uint8_t tw = 0;
    uint8_t th = 0;
    uint8_t tcc = 0;
    uint8_t tfx = 0;
    uint32_t cbp = 0;
    uint8_t cpsm = 0;
    uint8_t csm = 0;
    uint8_t csa = 0;
    uint8_t cld = 0;
};

struct GSXYOffsetReg
{
    uint16_t ofx = 0;
    uint16_t ofy = 0;
};

struct GSTexaReg
{
    uint8_t ta0 = 0;
    bool aem = false;
    uint8_t ta1 = 0;
};

struct GSTexClutReg
{
    uint8_t cbw = 0;
    uint8_t cou = 0;
    uint16_t cov = 0;
};

struct GSContext
{
    GSFrameReg frame;
    GSScissorReg scissor;
    GSTex0Reg tex0;
    GSXYOffsetReg xyoffset;
    GSZbufReg zbuf;
    uint64_t tex1 = 0;
    uint64_t miptbp1 = 0;
    uint64_t miptbp2 = 0;
    uint64_t clamp = 0;
    uint64_t alpha = 0;
    uint64_t test = 0;
    uint64_t fba = 0;
};

struct GSPrimReg
{
    GSPrimType type = GS_PRIM_POINT;
    bool iip = false;
    bool tme = false;
    bool fge = false;
    bool abe = false;
    bool aa1 = false;
    bool fst = false;
    bool ctxt = false;
    bool fix = false;
};

struct GSBitBltBuf
{
    uint32_t sbp = 0;
    uint8_t sbw = 0;
    uint8_t spsm = 0;
    uint32_t dbp = 0;
    uint8_t dbw = 0;
    uint8_t dpsm = 0;
};

struct GSTrxPos
{
    uint16_t ssax = 0;
    uint16_t ssay = 0;
    uint16_t dsax = 0;
    uint16_t dsay = 0;
    uint8_t dir = 0;
};

struct GSTrxReg
{
    uint16_t rrw = 0;
    uint16_t rrh = 0;
};

// GS mipmapping (TEX1, MIPTBP1, MIPTBP2), decoded per draw. maxLevel 0
// means no mipmapping: level 0 with the TEX1 filter as before
// (GSDrawState::linearFilter). With MXL > 0 and a mipmap MMIN (2-5) the
// level and filter come from the LOD of each pixel (gsMipSelect).
struct GSMipInfo
{
    uint8_t maxLevel = 0; // MXL (1-6)
    bool lcm = false;     // LOD = K (fixed) instead of log2(1/|Q|) << L + K
    uint8_t l = 0;
    float k = 0.0f;       // K, signed 7.4 fixed point
    bool magLinear = false; // MMAG: LOD < 0
    bool minLinear = false; // MMIN texel filter: LOD >= 0
    bool mipLinear = false; // MMIN 3/5: blend the two nearest levels
    std::array<uint32_t, 7> tbp{};
    std::array<uint8_t, 7> tbw{};
};

struct GSMipSample
{
    uint8_t level = 0;  // first level
    uint8_t level2 = 0; // second level (mipLinear)
    float frac = 0.0f;  // weight of level2
    bool linear = false;
};

inline uint32_t gsPsmBitsPerPixel(uint8_t psm)
{
    switch (psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_Z32:
    case GS_PSM_Z24:
    case GS_PSM_T8H:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
        return 32u;
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return 16u;
    case GS_PSM_T8:
        return 8u;
    case GS_PSM_T4:
        return 4u;
    default:
        return 32u;
    }
}

// TEX1 MXL > 0 with MMIN NEAREST_MIPMAP_* or LINEAR_MIPMAP_*: the levels'
// base pointers and widths. MTBA=1 derives levels 1-3 from level 0 as the
// GS manual describes (each level right after the previous one, half the
// buffer width); otherwise MIPTBP1 gives levels 1-3 and MIPTBP2 levels 4-6.
inline GSMipInfo gsDecodeMip(const GSTex0Reg &tex0, uint64_t tex1, uint64_t miptbp1, uint64_t miptbp2)
{
    GSMipInfo m{};
    const uint8_t mxl = static_cast<uint8_t>(std::min<uint64_t>((tex1 >> 2) & 0x7u, 6u));
    const uint8_t mmin = static_cast<uint8_t>((tex1 >> 6) & 0x7u);
    if (mxl == 0u || mmin < 2u || mmin > 5u)
        return m;
    m.maxLevel = mxl;
    m.lcm = (tex1 & 0x1u) != 0u;
    m.magLinear = ((tex1 >> 5) & 0x1u) != 0u;
    m.minLinear = mmin >= 4u;
    m.mipLinear = (mmin & 0x1u) != 0u;
    m.l = static_cast<uint8_t>((tex1 >> 19) & 0x3u);
    int32_t k = static_cast<int32_t>((tex1 >> 32) & 0xFFFu);
    if (k & 0x800)
        k -= 0x1000;
    m.k = static_cast<float>(k) / 16.0f;
    m.tbp[0] = tex0.tbp0;
    m.tbw[0] = tex0.tbw;
    for (uint32_t i = 0; i < 3u; ++i)
    {
        m.tbp[1u + i] = static_cast<uint32_t>((miptbp1 >> (i * 20u)) & 0x3FFFu);
        m.tbw[1u + i] = static_cast<uint8_t>((miptbp1 >> (i * 20u + 14u)) & 0x3Fu);
        m.tbp[4u + i] = static_cast<uint32_t>((miptbp2 >> (i * 20u)) & 0x3FFFu);
        m.tbw[4u + i] = static_cast<uint8_t>((miptbp2 >> (i * 20u + 14u)) & 0x3Fu);
    }
    if (((tex1 >> 9) & 0x1u) != 0u)
    {
        uint32_t w = 1u << std::min<uint32_t>(tex0.tw, 10u);
        uint32_t h = 1u << std::min<uint32_t>(tex0.th, 10u);
        const uint32_t bpp = gsPsmBitsPerPixel(tex0.psm);
        for (uint32_t level = 1; level <= 3u; ++level)
        {
            m.tbp[level] = (m.tbp[level - 1u] + (w * h * bpp / 8u) / 256u) & 0x3FFFu;
            m.tbw[level] = static_cast<uint8_t>(std::max<uint32_t>(1u, m.tbw[level - 1u] >> 1));
            w = std::max<uint32_t>(1u, w >> 1);
            h = std::max<uint32_t>(1u, h >> 1);
        }
    }
    return m;
}

// The level(s) and texel filter for a pixel with the given Q:
// LOD = (log2(1/|Q|) << L) + K (LCM=0) or K (LCM=1). LOD < 0 magnifies
// (MMAG, level 0); otherwise MMIN: NEAREST_MIPMAP rounds LOD to a level,
// LINEAR_MIPMAP blends floor(LOD) and the next level by its fraction.
// Levels stop at MXL.
inline GSMipSample gsMipSelect(const GSMipInfo &m, float q)
{
    GSMipSample out{};
    float lod = m.k;
    if (!m.lcm)
    {
        float aq = q < 0.0f ? -q : q;
        if (!(aq > 1.0e-30f))
            aq = 1.0e-30f;
        lod += -std::log2(aq) * static_cast<float>(1u << m.l);
    }
    if (!(lod >= 0.0f)) // also NaN
    {
        out.linear = m.magLinear;
        return out;
    }
    out.linear = m.minLinear;
    const float maxLevel = static_cast<float>(m.maxLevel);
    if (!m.mipLinear)
    {
        const float level = std::floor(lod + 0.5f);
        out.level = out.level2 = static_cast<uint8_t>(level >= maxLevel ? maxLevel : level);
        return out;
    }
    if (lod >= maxLevel)
    {
        out.level = out.level2 = m.maxLevel;
        return out;
    }
    const float level = std::floor(lod);
    out.level = static_cast<uint8_t>(level);
    out.level2 = static_cast<uint8_t>(out.level + 1u);
    out.frac = lod - level;
    return out;
}

struct GSDrawState
{
    GSContext context{};
    GSPrimReg prim{};
    GSTexaReg texa{};
    GSTexClutReg texclut{};
    bool pabe = false;
    uint64_t scanmsk = 0;
    uint64_t dimx = 0;
    uint64_t dthe = 0;
    uint64_t colclamp = 0;
    uint8_t fogR = 0;
    uint8_t fogG = 0;
    uint8_t fogB = 0;
    uint16_t textureWidth = 1;
    uint16_t textureHeight = 1;
    bool linearFilter = false;
    GSMipInfo mip{};
};

struct GSPrimitiveBatch
{
    std::array<GSVertex, 3> vertices{};
    uint8_t vertexCount = 0;
    GSDrawState state{};
};

struct GSTransferCommand
{
    GSBitBltBuf bitbltbuf{};
    GSTrxPos trxpos{};
    GSTrxReg trxreg{};
    uint32_t direction = 3;
};

struct GSTransferSnapshot
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t totalPixels = 0;
    uint32_t copiedPixels = 0;
    uint32_t direction = 3;
    size_t localToHostPendingBytes = 0;
};

// Pictures a backend keeps for the window at once (GS's frame queue: the
// one shown, two game frames waiting, one being made).
constexpr uint32_t kPresentationSlots = 4;

struct GSPresentationRequest
{
    // The picture to make: 0 to kPresentationSlots - 1 (a backend that keeps
    // its picture on the GPU has one texture per slot).
    uint32_t slot = 0;
    uint64_t pmode = 0;
    uint64_t smode2 = 0;
    uint64_t dispfb1 = 0;
    uint64_t display1 = 0;
    uint64_t dispfb2 = 0;
    uint64_t display2 = 0;
    uint64_t bgcolor = 0;
    uint64_t vsyncTick = 0;
    // The game frame's number when the frame queue makes it (GS::capturePresentationFrame), else 0: frame
    // interpolation keeps such frames (GSRasterBackend::PresentBetween).
    uint64_t frame = 0;
    GSFrameReg contextFrames[2]{};
    GSFrameReg preferredSource{};
    uint32_t preferredDestFbp = 0;
    bool hasPreferredSource = false;
};

struct PresentationFrame
{
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t displayFbp = 0;
    uint32_t sourceFbp = 0;
    bool usedPreferred = false;
    // The picture stays on the host's GPU device (no pixels), in the
    // request's slot: see GSRasterBackend::PresentedGpuPicture.
    bool onGpu = false;

    explicit operator bool() const
    {
        return (onGpu || !pixels.empty()) && width != 0u && height != 0u;
    }
};

enum class GSSyncReason : uint8_t
{
    Finish,
    LocalToHost,
    Presentation,
    DebugReadback,
    Reset,
};
