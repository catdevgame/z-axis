#pragma once

// GS register words the engine builds on the EE (GS User's Manual 7.2):
// the registers' numbers, the GIF tag that heads a block of register
// writes, PRIM's primitive types and attributes, TEX0 (texture base,
// buffer width, format, size, CLUT) and MIPTBP1 (the base pointers of mip
// levels 1-3). Texture and CLUT addresses are GS block numbers (256 bytes,
// 64 words): a word address >> 6.
//
// The words are made as the game makes them: each field is shifted into
// place and OR-ed, not masked to its width. With the values the engine
// gives (addresses within GS memory, 8- and 16-bit fields that fit)
// that is the hardware layout; a value too wide spills into the next
// field, as in the game.

#include <cstdint>
#include <initializer_list>

namespace zge::gs
{
    // The GS registers the engine writes, by their numbers (the GS User's
    // Manual's addresses: what an address and data pair names; in a GIF
    // tag's register list PRIM to CLAMP_2 have the same numbers). _1 and _2
    // are drawing contexts 1 and 2.
    constexpr uint32_t kPrim = 0x00;
    constexpr uint32_t kRgbaq = 0x01;
    constexpr uint32_t kSt = 0x02;
    constexpr uint32_t kUv = 0x03;
    constexpr uint32_t kXyzf2 = 0x04;
    constexpr uint32_t kXyz2 = 0x05;
    constexpr uint32_t kTex0_1 = 0x06;
    constexpr uint32_t kTex0_2 = 0x07;
    constexpr uint32_t kClamp1 = 0x08;
    constexpr uint32_t kClamp2 = 0x09;
    constexpr uint32_t kTex1_1 = 0x14;
    constexpr uint32_t kTex1_2 = 0x15;
    constexpr uint32_t kXyOffset1 = 0x18;
    constexpr uint32_t kPrModeCont = 0x1A;
    constexpr uint32_t kMipTbp1_1 = 0x34;
    constexpr uint32_t kTexA = 0x3B;
    constexpr uint32_t kFogCol = 0x3D;
    constexpr uint32_t kTexFlush = 0x3F;
    constexpr uint32_t kScissor1 = 0x40;
    constexpr uint32_t kAlpha1 = 0x42;
    constexpr uint32_t kAlpha2 = 0x43;
    constexpr uint32_t kColClamp = 0x46;
    constexpr uint32_t kTest1 = 0x47;
    constexpr uint32_t kTest2 = 0x48;
    constexpr uint32_t kPabe = 0x49;
    constexpr uint32_t kFba1 = 0x4A;
    constexpr uint32_t kFrame1 = 0x4C;
    constexpr uint32_t kFrame2 = 0x4D;
    constexpr uint32_t kZbuf1 = 0x4E;
    constexpr uint32_t kZbuf2 = 0x4F;
    constexpr uint32_t kBitBltBuf = 0x50;
    constexpr uint32_t kTrxPos = 0x51;
    constexpr uint32_t kTrxReg = 0x52;
    constexpr uint32_t kTrxDir = 0x53;

    // In a GIF tag's register list: the next quadword is an address and
    // data pair (A+D: its data in the low doubleword, the register's number in
    // the high).
    constexpr uint32_t kAddressData = 0x0E;

    // A GIF tag's register list (REGS, the tag's second doubleword): a
    // register a nibble, the first lowest.
    constexpr uint64_t registerList(std::initializer_list<uint32_t> registers)
    {
        uint64_t list = 0;
        uint32_t shift = 0;
        for (const uint32_t r : registers)
        {
            list |= static_cast<uint64_t>(r) << shift;
            shift += 4;
        }
        return list;
    }

    // How the data after a GIF tag is read (FLG).
    constexpr uint32_t kPacked = 0;       // a quadword per register of the list
    constexpr uint32_t kRegisterList = 1; // a doubleword per register of the list
    constexpr uint32_t kImage = 2;        // texels for a transfer to GS memory, NLOOP quadwords

    // A GIF tag's first doubleword (GS User's Manual 3.3): how the block
    // after it is read. Bits 32-45 are not the GIF's: the engine's VU1
    // programs keep their own data there.
    struct GifTag
    {
        uint64_t loops = 0;       // NLOOP, bits 0-14: times the register list repeats (a strip's vertices)
        uint64_t endOfPacket = 0; // EOP, bit 15: the transfer's last tag
        uint64_t setsPrim = 0;    // PRE, bit 46: PRIM is written from the tag's prim first
        uint64_t prim = 0;        // PRIM, bits 47-57 (with PRE)
        uint64_t format = 0;      // FLG, bits 58-59 (kPacked, kRegisterList, kImage)
        uint64_t registers = 0;   // NREG, bits 60-63: the register list's length

        constexpr uint64_t bits() const
        {
            return loops | endOfPacket << 15 | setsPrim << 46 | prim << 47 | format << 58 | registers << 60;
        }
    };
    // A register list's first register (its low nibble).
    constexpr uint64_t kFirstRegister = 0xFull;
    // The tag's fields' bits in its first doubleword.
    constexpr uint64_t kGifTagLoops = 0x7FFFull;          // NLOOP
    constexpr uint64_t kGifTagSetsPrim = 1ull << 46;      // PRE
    constexpr uint64_t kGifTagFormat = 3ull << 58;        // FLG
    constexpr uint64_t kGifTagRegisters = 0xFull << 60;   // NREG

    // PRIM (and a GIF tag's prim): the primitive type in bits 0-2, then its
    // attributes.
    constexpr uint32_t kPoint = 0, kLine = 1, kLineStrip = 2, kTriangle = 3, kTriangleStrip = 4, kTriangleFan = 5,
                       kSprite = 6;
    constexpr uint32_t kGouraud = 1u << 3;     // IIP: colours shaded across the primitive
    constexpr uint32_t kTextured = 1u << 4;    // TME
    constexpr uint32_t kFogged = 1u << 5;      // FGE
    constexpr uint32_t kBlended = 1u << 6;     // ABE: alpha blending
    constexpr uint32_t kTexelCoords = 1u << 8; // FST: the texture coordinates are UV (texels), not ST
    constexpr uint32_t kContext2 = 1u << 9;    // CTXT: drawn with context 2's registers
    constexpr uint32_t kPrimBlendShift = 6, kPrimContextShift = 9;
    constexpr uint32_t kPrimAttributes = 0x7F8u; // bits 3-10: every attribute, the type left out

    // Pixel storage formats (PSM, CPSM) the engine uses.
    enum Psm : uint8_t
    {
        kPsmCt32 = 0x00,
        kPsmCt24 = 0x01,
        kPsmCt16 = 0x02,
        kPsmCt16S = 0x0A,
        kPsmT8 = 0x13,
        kPsmT4 = 0x14,
        kPsmZ16S = 0x3A, // a depth buffer's (ZBUF, libgraph's zpsm)
    };

    // Words per GS block (the unit of TBP / CBP) and per page (8 KB, the
    // unit a buffer of another format must start on, and of FBP and ZBP).
    constexpr uint32_t kWordsPerBlock = 0x40u;
    constexpr uint32_t kWordsPerPage = 0x800u;
    constexpr uint32_t blockOf(uint32_t wordAddress) { return wordAddress >> 6; }
    constexpr uint32_t pageOf(uint32_t wordAddress) { return wordAddress >> 11; }
    // GS memory: 4 MB, in words.
    constexpr uint32_t kMemoryWords = 1u << 20;

    // FRAME's FBP (bits 0-8): the frame buffer's base, in pages.
    constexpr uint32_t kFrameBaseBits = 0x1FFu;
    template <typename T>
    constexpr T frameBasePage(T frame)
    {
        return frame & kFrameBaseBits;
    }
    // A page's first block (32 blocks a page): what TBP0 and an image
    // transfer's address take.
    template <typename T>
    constexpr T blockOfPage(T page)
    {
        return page << 5;
    }
    // A buffer's width in the GS's units of 64 pixels (FBW, TBW, an image
    // transfer's): rounded towards zero.
    template <typename T>
    constexpr T bufferWidthUnits(T pixels)
    {
        return pixels / 64;
    }

    // A field of a register's word: its first bit and its width.
    struct Field
    {
        uint32_t shift, bits;

        // The field's bits in the word.
        constexpr uint64_t mask() const { return ((1ull << bits) - 1) << shift; }
        // A value in the field's place, not masked to its width (as the game
        // ORs one in).
        constexpr uint64_t placed(uint64_t value) const { return value << shift; }
    };
    // The word with the field set to the value (masked to the field's
    // width), and the field's value.
    constexpr uint64_t withField(uint64_t word, Field field, uint64_t value)
    {
        return (word & ~field.mask()) | (field.placed(value) & field.mask());
    }
    constexpr uint64_t fieldOf(uint64_t word, Field field) { return word >> field.shift & ((1ull << field.bits) - 1); }

    // FRAME: the frame buffer a context draws to.
    struct Frame
    {
        static constexpr Field kFbp{0, 9};    // its base, in pages (kWordsPerPage)
        static constexpr Field kFbw{16, 6};   // its width / 64 pixels
        static constexpr Field kPsm{24, 6};   // its pixel format (Psm)
        static constexpr Field kFbmsk{32, 32}; // the bits not written (a set bit kept)
    };

    // ZBUF: the depth buffer a context draws to.
    struct Zbuf
    {
        static constexpr Field kZmsk{32, 1}; // depth not written (1)
    };

    struct Tex0
    {
        static constexpr Field kTbp0{0, 14}; // the texture's base (block)
        static constexpr Field kTbw{14, 6};  // its buffer width / 64 pixels
        static constexpr Field kPsm{20, 6};  // its pixel format (Psm)
        static constexpr Field kTw{26, 4};   // log2 of its width
        static constexpr Field kTh{30, 4};   // log2 of its height
        static constexpr Field kTcc{34, 1}; // the texture's alpha used (1) or not
        static constexpr Field kTfx{35, 2}; // how it combines with the colour: 0 modulate
        static constexpr Field kCbp{37, 14}; // the CLUT's base (block)
        static constexpr Field kCpsm{51, 4}; // its format
        static constexpr Field kCsm{55, 1};  // its storage mode (kClutCsm1)
        static constexpr Field kCsa{56, 5};  // its first entry
        static constexpr Field kCld{61, 3};  // when the GS loads it (kClutLoadAlways)

        uint32_t tbp0 = 0; // bits 0-13   texture base (block)
        uint32_t tbw = 0;  // bits 14-19  buffer width / 64 pixels
        uint32_t psm = 0;  // bits 20-25  pixel format
        uint32_t tw = 0;   // bits 26-29  log2 width
        uint32_t th = 0;   // bits 30-33  log2 height
        uint32_t tcc = 0;  // bit 34      1: RGBA (use the texture's alpha)
        uint32_t tfx = 0;  // bits 35-36  0: modulate
        uint32_t cbp = 0;  // bits 37-50  CLUT base (block)
        uint32_t cpsm = 0; // bits 51-54  CLUT format
        uint32_t csm = 0;  // bit 55      CLUT storage mode (0: CSM1)
        uint32_t csa = 0;  // bits 56-60  CLUT entry offset
        uint32_t cld = 0;  // bits 61-63  CLUT buffer load control (1: always load)

        constexpr uint64_t bits() const
        {
            return static_cast<uint64_t>(tbp0) | static_cast<uint64_t>(tbw) << 14 | static_cast<uint64_t>(psm) << 20 |
                   static_cast<uint64_t>(tw) << 26 | static_cast<uint64_t>(th) << 30 | static_cast<uint64_t>(tcc) << 34 |
                   static_cast<uint64_t>(tfx) << 35 | static_cast<uint64_t>(cbp) << 37 | static_cast<uint64_t>(cpsm) << 51 |
                   static_cast<uint64_t>(csm) << 55 | static_cast<uint64_t>(csa) << 56 | static_cast<uint64_t>(cld) << 61;
        }
    };

    constexpr uint32_t kClutCsm1 = 0;       // CSM: the CLUT stored as CSM1 (in blocks)
    constexpr uint32_t kClutLoadAlways = 1; // CLD: the CLUT loaded with every TEX0 written

    // CLAMP: how texture coordinates wrap.
    struct Clamp
    {
        static constexpr Field kWms{0, 2};   // horizontally (kWrapRepeat, kWrapClamp; 2 and 3: by region)
        static constexpr Field kWmt{2, 2};   // vertically
        static constexpr Field kMinU{4, 10}; // the regions' bounds
        static constexpr Field kMaxU{14, 10};
        static constexpr Field kMinV{24, 10};
        static constexpr Field kMaxV{34, 10};
    };
    constexpr uint32_t kWrapRepeat = 0, kWrapClamp = 1;

    // TEX1: the texture's filtering and mip levels.
    struct Tex1
    {
        static constexpr Field kLcm{0, 1};  // the level by K alone (1) or by depth
        static constexpr Field kMxl{2, 3};  // the last mip level
        static constexpr Field kMmag{5, 1}; // magnified: 0 nearest, 1 linear
        static constexpr Field kMmin{6, 3}; // minified (kMinLinear, kMinLinearMipLinear)
        static constexpr Field kMtba{9, 1}; // levels 1-3's bases worked out by the GS
        static constexpr Field kL{19, 2};   // the level's scale
        static constexpr Field kK{32, 12};  // the level's offset
    };
    constexpr uint32_t kMinLinear = 1, kMinLinearMipLinear = 5;

    // ALPHA: the blend, (A - B) * C / 0x80 + D. A, B and D take a colour
    // (kBlendSource, kBlendFrame, kBlendZero), C a factor (kBlendSourceAlpha,
    // kBlendFrameAlpha, kBlendFix: FIX).
    struct Alpha
    {
        uint32_t a = 0, b = 0, c = 0, d = 0;
        uint32_t fix = 0;

        static constexpr Field kA{0, 2};
        static constexpr Field kB{2, 2};
        static constexpr Field kC{4, 2};
        static constexpr Field kD{6, 2};
        static constexpr Field kInputs{0, 8}; // A, B, C and D
        static constexpr Field kFix{32, 8};

        constexpr uint64_t bits() const
        {
            return static_cast<uint64_t>(a) | static_cast<uint64_t>(b) << 2 | static_cast<uint64_t>(c) << 4 |
                   static_cast<uint64_t>(d) << 6 | static_cast<uint64_t>(fix) << 32;
        }
    };
    constexpr uint32_t kBlendSource = 0, kBlendFrame = 1, kBlendZero = 2;
    constexpr uint32_t kBlendSourceAlpha = 0, kBlendFrameAlpha = 1, kBlendFix = 2;

    // TEST: the alpha test, the destination alpha test and the depth test.
    struct Test
    {
        uint32_t ate = 0, atst = 0, aref = 0, afail = 0, date = 0, datm = 0, zte = 0, ztst = 0;

        static constexpr Field kAte{0, 1};    // the alpha test on
        static constexpr Field kAtst{1, 3};   // its comparison (kAlphaGreater)
        static constexpr Field kAref{4, 8};   // the alpha it compares with
        static constexpr Field kAfail{12, 2}; // what a pixel failing it still writes (kFailFrameOnly)
        static constexpr Field kDate{14, 1};  // the destination alpha test on
        static constexpr Field kDatm{15, 1};  // which destination alpha passes
        static constexpr Field kZte{16, 1};   // the depth test on
        static constexpr Field kZtst{17, 2};  // its comparison (kDepthAlways, kDepthGequal, kDepthGreater)

        constexpr uint64_t bits() const
        {
            return static_cast<uint64_t>(ate) | static_cast<uint64_t>(atst) << 1 | static_cast<uint64_t>(aref) << 4 |
                   static_cast<uint64_t>(afail) << 12 | static_cast<uint64_t>(date) << 14 | static_cast<uint64_t>(datm) << 15 |
                   static_cast<uint64_t>(zte) << 16 | static_cast<uint64_t>(ztst) << 17;
        }
    };
    constexpr uint32_t kAlphaGreater = 6;
    constexpr uint32_t kFailFrameOnly = 1; // a failing pixel writes the frame buffer, not the depth
    constexpr uint32_t kDepthAlways = 1, kDepthGequal = 2, kDepthGreater = 3;

    // COLCLAMP: colours past 0-255 clamped (1), not wrapped to their low 8
    // bits.
    constexpr uint32_t kColoursClamped = 1;
    // PRMODECONT: the primitives' attributes taken from PRIM (1), not PRMODE.
    constexpr uint32_t kAttributesFromPrim = 1;

    // RGBAQ: a vertex's colour and Q (a float's bits).
    struct Rgbaq
    {
        uint32_t r = 0, g = 0, b = 0, a = 0;
        uint32_t q = 0;

        constexpr uint64_t bits() const
        {
            return static_cast<uint64_t>(r) | static_cast<uint64_t>(g) << 8 | static_cast<uint64_t>(b) << 16 |
                   static_cast<uint64_t>(a) << 24 | static_cast<uint64_t>(q) << 32;
        }
    };

    // XYZF2: a vertex's position (X and Y in subpixels, fixed_point.h), depth
    // and fog.
    struct Xyzf
    {
        uint32_t x = 0, y = 0, z = 0, f = 0;

        constexpr uint64_t bits() const
        {
            return static_cast<uint64_t>(x) | static_cast<uint64_t>(y) << 16 | static_cast<uint64_t>(z) << 32 |
                   static_cast<uint64_t>(f) << 56;
        }
    };

    // SCISSOR: the pixels a context draws, from the first to the last both
    // ways (in pixels of the frame buffer).
    struct Scissor
    {
        uint32_t left = 0, right = 0, top = 0, bottom = 0; // SCAX0, SCAX1, SCAY0, SCAY1
        static constexpr Field kLeft{0, 11}, kRight{16, 11}, kTop{32, 11}, kBottom{48, 11};

        constexpr uint64_t bits() const
        {
            return static_cast<uint64_t>(left) | static_cast<uint64_t>(right) << 16 | static_cast<uint64_t>(top) << 32 |
                   static_cast<uint64_t>(bottom) << 48;
        }
    };

    // TEXA: the alpha a 16- or 24-bit texel's is read as: TA0 (a 16-bit
    // texel's with its alpha bit clear, and a 24-bit texel's), TA1 (with it
    // set); AEM, a black texel's alpha 0.
    struct TexA
    {
        uint32_t ta0 = 0, aem = 0, ta1 = 0;

        constexpr uint64_t bits() const
        {
            return static_cast<uint64_t>(ta0) | static_cast<uint64_t>(aem) << 15 | static_cast<uint64_t>(ta1) << 32;
        }
    };

    // BITBLTBUF: a transfer's source and destination in GS memory.
    struct BitBltBuf
    {
        static constexpr Field kDbp{32, 14}; // the destination's base (block)
        static constexpr Field kDbw{48, 6};  // its buffer width / 64 pixels
        static constexpr Field kDpsm{56, 6}; // its pixel format
    };
    // TRXREG: the transfer's rectangle, in pixels.
    struct TrxReg
    {
        static constexpr Field kRrw{0, 12};  // its width
        static constexpr Field kRrh{32, 12}; // its height
    };
    // TRXDIR: the transfer's direction; the engine's go from the EE to GS
    // memory.
    constexpr uint32_t kHostToLocal = 0;

    // A 256-entry CLUT stored as CSM1 has bits 3 and 4 of each index
    // swapped: entry csm1Entry(i) sits at place i.
    constexpr uint32_t csm1Entry(uint32_t place) { return (place & 0x8u) << 1 | (place & 0x10u) >> 1 | (place & ~0x18u); }

    // XYOFFSET: the drawing space's offset from the frame buffer's corner
    // (12.4 fixed point, fixed_point.h).
    struct XyOffset
    {
        static constexpr Field kOfx{0, 16};
        static constexpr Field kOfy{32, 16};
    };

    struct MipTbp1
    {
        // The bases (blocks) and buffer widths (/ 64 pixels) of mip levels 1,
        // 2 and 3.
        static constexpr Field kTbp1{0, 14};
        static constexpr Field kTbw1{14, 6};
        static constexpr Field kTbp2{20, 14};
        static constexpr Field kTbw2{34, 6};
        static constexpr Field kTbp3{40, 14};
        static constexpr Field kTbw3{54, 6};

        uint32_t tbp1 = 0; // bits 0-13   level 1 base (block)
        uint32_t tbw1 = 0; // bits 14-19  level 1 buffer width
        uint32_t tbp2 = 0; // bits 20-33
        uint32_t tbw2 = 0; // bits 34-39
        uint32_t tbp3 = 0; // bits 40-53
        uint32_t tbw3 = 0; // bits 54-59

        uint64_t bits() const
        {
            return static_cast<uint64_t>(tbp1) | static_cast<uint64_t>(tbw1) << 14 | static_cast<uint64_t>(tbp2) << 20 |
                   static_cast<uint64_t>(tbw2) << 34 | static_cast<uint64_t>(tbp3) << 40 | static_cast<uint64_t>(tbw3) << 54;
        }
    };
}
