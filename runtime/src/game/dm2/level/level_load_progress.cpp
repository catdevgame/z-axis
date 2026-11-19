// The load screen's progress bar (level_load_progress.h): DM2's own code
// (its game block, in the physics.c anchor's range).

#include "level_load_progress.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libc.h"
#include "sce/libgcc.h"
#include "zge/assets/file.h"
#include "zge/math/conversions.h"
#include "zge/render/display.h"
#include "zge/text/cstring.h"

#include <bit>
#include <cstdint>
#include "sym/dm2/level/level_load_progress.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr uint32_t kGeometryExtension = sym::g_textGeo; // "GEO"
        constexpr uint32_t kTextureExtension = sym::g_textTxr;  // "TXR"
        constexpr uint32_t kDataExtension = sym::g_textDat;     // "DAT"
        constexpr uint32_t kInstanceExtension = sym::g_textIns; // "INS"
        constexpr uint32_t kMotionName = sym::g_textMotion;     // "MOTION"
        constexpr uint32_t kNpcName = sym::g_textNpc;           // "NPC"
        constexpr uint32_t kOverlayName = sym::g_textOverlay;   // "OVERLAY"
        constexpr uint32_t kFxName = sym::g_textFx;             // "FX"
        constexpr uint32_t kFinalName = sym::g_textFinal;       // "FINAL"

        // The motion files are numbered down to 0 by their extension: the
        // highest number seen is how many there are.
        inline ee::Global<uint32_t, sym::g_motionFileCount> g_motionFileCount;

        // What each part weighs in the bar (they add up to 1). The original
        // adds them up in double precision.
        constexpr uint64_t bits(double value) { return std::bit_cast<uint64_t>(value); }
        constexpr uint64_t kPartWeights[kLevelLoadParts] = {
            bits(0.35), bits(0.25), bits(0.025), bits(0.025), bits(0.15), bits(0.05), bits(0.05), bits(0.05), bits(0.05),
        };
        constexpr uint64_t kWholeBar = bits(1.0);

        // The bar on the load screen.
        constexpr uint32_t kBarX = 173, kBarY = 353, kBarHeight = 22;
        constexpr float kBarWidth = 340.0f;
        constexpr ee::Ptr<uint8_t> kBarColour{sym::g_loadBarColour};

        ee::Float fractionDone(uint32_t done, uint32_t total)
        {
            return zge::unsignedToFloat(done) / zge::unsignedToFloat(total);
        }
    }

    void levelLoadProgress(ee::Ptr<const char> path, uint32_t bytesDone, uint32_t totalBytes)
    {
        ee::Local<char[0x10]> extension;
        ee::Local<char[0x40]> name;

        zge::pathExtension(path, extension);
        zge::pathBaseName(path, name);
        ee::Float *const parts = *g_levelLoadParts;
        if (zge::stringCompareNoCase(extension, kGeometryExtension) == 0)
            parts[kLoadGeometry] = fractionDone(bytesDone, totalBytes);
        else if (zge::stringCompareNoCase(extension, kTextureExtension) == 0)
            parts[kLoadTextures] = fractionDone(bytesDone, totalBytes);
        else if (zge::stringCompareNoCase(extension, kDataExtension) == 0)
            parts[kLoadData] = fractionDone(bytesDone, totalBytes);
        else if (zge::stringCompareNoCase(extension, kInstanceExtension) == 0)
            parts[kLoadInstances] = fractionDone(bytesDone, totalBytes);
        else if (zge::stringCompareNoCase(name, kMotionName) == 0)
        {
            const uint32_t number = static_cast<uint32_t>(sce::libc::atoi(extension));
            if (*g_motionFileCount < number)
                *g_motionFileCount = number;
            parts[kLoadMotions] = fractionDone(*g_motionFileCount - number, *g_motionFileCount);
        }
        else if (zge::stringCompareNoCase(name, kNpcName) == 0)
            parts[kLoadNpc] = fractionDone(bytesDone, totalBytes);
        else if (zge::stringCompareNoCase(name, kOverlayName) == 0)
            parts[kLoadOverlay] = fractionDone(bytesDone, totalBytes);
        else if (zge::stringCompareNoCase(name, kFxName) == 0)
            parts[kLoadFx] = fractionDone(bytesDone, totalBytes);
        else if (zge::stringCompareNoCase(name, kFinalName) == 0)
            parts[kLoadFinal] = fractionDone(bytesDone, totalBytes);
        else
            return;

        uint64_t sum = sce::libgcc::dpmul(sce::libgcc::fptodp(parts[0]), kPartWeights[0]);
        for (uint32_t part = 1; part < kLevelLoadParts; ++part)
        {
            const uint64_t weighted = sce::libgcc::dpmul(sce::libgcc::fptodp(parts[part]), kPartWeights[part]);
            sum = sce::libgcc::dpadd(sum, weighted);
        }
        ee::Float done = sce::libgcc::dptofp(sum);
        if (sce::libgcc::dpcmp(sce::libgcc::fptodp(done), kWholeBar) > 0)
            done = 1.0f;
        zge::clearDisplayRectangle(kBarX, kBarY, sce::libgcc::fptoui(done * ee::Float(kBarWidth)), kBarHeight, kBarColour);
    }
    READABLE_FUNCTION(levelLoadProgress, Verified);
}
