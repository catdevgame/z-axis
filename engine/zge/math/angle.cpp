#include "angle.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "sce/libm.h"

#include <array>
#include <bit>
#include "sym/zge/math/angle.cpp.h"


namespace zge
{
    // ---- ZGE2's tables ---------------------------------------------------------------
    //
    // ZGE2 looks its trigonometry up in tables initMathUtilities builds once (ZGE3 calls the C library each time):
    // the sine of every eighth angle over a turn and a quarter (the cosine is the same table a quarter turn on), and
    // the arc sine and arc tangent, as angles, of every 1/4096 from -1 to 1 (each table reached through a pointer to
    // its middle, x = 0). Read in DM2's angleSin .. angleAtan2 and the three builders.
    namespace
    {
        constexpr uint32_t kSineEntries = 0x2800;      // a turn (0x2000 entries) and a quarter
        constexpr uint32_t kQuarterTurnEntries = 0x800;
        constexpr int32_t kAngleUnitsAnEntry = 8;      // an angle's low three bits are dropped
        constexpr uint32_t kTurnEntryMask = 0x1FFF;
        constexpr int32_t kArcSteps = 0x1000;          // the arc tables' steps from 0 to 1
        constexpr ee::Float kArcStep = std::bit_cast<float>(0x39800000u);       // 1/4096
        constexpr ee::Float kArcStepsAUnit = std::bit_cast<float>(0x45800000u); // 4096
        constexpr ee::Float kTangentLimit = std::bit_cast<float>(0x47C34FFFu);  // 99999.99: where the cosine is about 0
        constexpr int32_t kQuarterTurn = 0x4000;
        constexpr int32_t kHalfTurn = 0x8000;
        using ArcTable = std::array<int16_t, 2 * kArcSteps + 1>;

        inline ee::Global<std::array<ee::Float, kSineEntries>, sym::dm2::g_sineTable> g_sineTable;
        inline ee::Global<ee::Ptr<ee::Float>, sym::dm2::g_cosineTable> g_cosineTable; // the sine table a quarter turn on
        inline ee::Global<ArcTable, sym::dm2::g_arcSineValues> g_arcSineValues;
        inline ee::Global<ee::Ptr<int16_t>, sym::dm2::g_arcSineTable> g_arcSineTable; // the middle of g_arcSineValues
        inline ee::Global<ArcTable, sym::dm2::g_arcTangentValues> g_arcTangentValues;
        inline ee::Global<ee::Ptr<int16_t>, sym::dm2::g_arcTangentTable> g_arcTangentTable; // the middle of g_arcTangentValues

        // The sine table's entry for an angle (a whole turn of them).
        uint32_t turnEntry(Angle angle) { return (static_cast<uint32_t>(angle) / kAngleUnitsAnEntry) & kTurnEntryMask; }
        // An arc table's value at x (from -1 to 1), through the pointer to its middle.
        Angle arcEntry(ee::Ptr<int16_t> middle, ee::Float x) { return middle[ee::toInt(x * kArcStepsAUnit)]; }
    }

    namespace // the game functions below call: ee/plain.h
    {
        ee::Float sinf(ee::Float x) { return ee::game<ee::Float>(kSinf, x); }
    }

    // buildSineTable: sinf of every eighth angle, a turn and a quarter of them; the cosine table pointed a quarter on.
    inline void buildSineTable()
    {
        *g_cosineTable = ee::Ptr<ee::Float>(&(*g_sineTable)[kQuarterTurnEntries]);
        for (uint32_t i = 0; i < kSineEntries; ++i)
            (*g_sineTable)[i] = sinf(ee::toFloat(static_cast<Angle>(i * kAngleUnitsAnEntry)) * kAngleToRadians);
    }
    READABLE_FUNCTION_ONLY(dm2, buildSineTable);

    // buildArcSineTable: asinf of every 1/4096 from -1 to 1, as an angle.
    inline void buildArcSineTable()
    {
        *g_arcSineTable = ee::Ptr<int16_t>(&(*g_arcSineValues)[kArcSteps]);
        for (int32_t i = 0; i <= 2 * kArcSteps; ++i)
            (*g_arcSineValues)[static_cast<uint32_t>(i)] = radiansToAngle(sce::libm::asinf(ee::toFloat(i - kArcSteps) * kArcStep));
    }
    READABLE_FUNCTION_ONLY(dm2, buildArcSineTable);

    // buildArcTangentTable: atanf of every 1/4096 from -1 to 1, as an angle.
    inline void buildArcTangentTable()
    {
        *g_arcTangentTable = ee::Ptr<int16_t>(&(*g_arcTangentValues)[kArcSteps]);
        for (int32_t i = 0; i <= 2 * kArcSteps; ++i)
            (*g_arcTangentValues)[static_cast<uint32_t>(i)] = radiansToAngle(sce::libm::atanf(ee::toFloat(i - kArcSteps) * kArcStep));
    }
    READABLE_FUNCTION_ONLY(dm2, buildArcTangentTable);

    // sineFromTable, cosineFromTable: ZGE2's angleSin and angleCos, the table's entry.
    ee::Float sineFromTable(Angle angle) { return (*g_sineTable)[turnEntry(angle)]; }
    ee::Float cosineFromTable(Angle angle) { return (*g_cosineTable)[turnEntry(angle)]; }

    // tangentFromTables: ZGE2's angleTan, sine over cosine (kTangentLimit where the cosine is about 0).
    ee::Float tangentFromTables(Angle angle)
    {
        const ee::Float cosine = angleCos(angle);
        if (ee::abs(cosine) < kMathEpsilon)
            return kTangentLimit;
        return angleSin(angle) / cosine;
    }

    // arcSineFromTable: ZGE2's angleAsin, x held to -1 .. 1 first.
    Angle arcSineFromTable(ee::Float x)
    {
        return arcEntry(*g_arcSineTable, ee::max(ee::min(x, 1.0f), -1.0f));
    }

    // arcCosineFromTable: ZGE2's angleAcos, a quarter turn less the arc sine.
    Angle arcCosineFromTable(ee::Float x)
    {
        return static_cast<Angle>(kQuarterTurn - angleAsin(x));
    }

    // arcTangentFromTable: ZGE2's angleAtan; past -1 .. 1 through 1/x (atan x = +-quarter turn - atan 1/x).
    Angle arcTangentFromTable(ee::Float x)
    {
        if (1.0f < x)
            return static_cast<Angle>(kQuarterTurn - arcEntry(*g_arcTangentTable, 1.0f / x));
        if (x < -1.0f)
            return static_cast<Angle>(-kQuarterTurn - arcEntry(*g_arcTangentTable, 1.0f / x));
        return arcEntry(*g_arcTangentTable, x);
    }

    // arcTangent2FromTable: ZGE2's angleAtan2, from the arc tangent of the smaller side over the larger, both made
    // positive: a quarter turn less it when y is the larger, a half turn on when x is negative, the whole negated
    // when one of the two is.
    Angle arcTangent2FromTable(ee::Float y, ee::Float x)
    {
        int32_t negate = 0;
        bool fromY = false;
        int32_t halfTurn = 0;
        if (y < 0.0f)
        {
            y = ee::abs(y);
            negate = -1;
        }
        if (x < 0.0f)
        {
            x = ee::abs(x);
            halfTurn = kHalfTurn;
            negate += 1;
        }
        ee::Float ratio;
        if (x < y)
        {
            ratio = x / y;
            fromY = true;
        }
        else
            ratio = y / x;
        Angle angle = angleAtan(ratio);
        if (fromY)
            angle = static_cast<Angle>(kQuarterTurn - angle);
        angle = static_cast<Angle>(angle + halfTurn);
        if (negate)
            angle = static_cast<Angle>(-angle);
        return angle;
    }

    // ---- ZGE4's VU0 microprogram ---------------------------------------------------------
    //
    // ZGE4 (BMX XXX) has VU0 work its sine and cosine out: initPlatform uploads a microprogram to VU0's code memory
    // (loadVu0Microprogram), and angleSin and angleCos hand it the angle in vf1's x and start it at its address 0
    // (VCALLMS 0). It turns the angle to radians, puts radians - pi/2 in x and the radians in y, and gives each lane
    // its cosine with a polynomial: the sine in x, the cosine in y. Its steps are VU0's (rounding towards zero), in
    // its order; the GameCube and Xbox builds call the C library instead.
    //
    // What it leaves in VU0 is the PS2's VU0 state, and the game's later vector code reads it: the angle reaches VU0
    // by QMTC2, which moves all 128 bits of its register, so z and w hold that register's upper half (what the last
    // 128-bit move through it left: string bytes, a pointer, 0), and the program runs the same cosine on them. Every
    // lane's steps stay in vf1 .. vf6 and ACC, with the program's flags, and the engine's vector macros that set x, y
    // and z only (a vector scaled into vf6, then stored whole) carry those w lanes into game memory. So the program's
    // four lanes are worked out here and left in VU0 as it leaves them (VI15, the I register and VU0's program
    // counter it leaves too are read by nothing).
    namespace
    {
        // The microprogram's constants, the values it loads into VU0's I register.
        constexpr ee::Float kQuarterTurnRadians = std::bit_cast<float>(0x3FC90FDBu);   // pi / 2
        constexpr ee::Float kMinusTurnsARadian = std::bit_cast<float>(0xBE22F983u);    // -1 / 2 pi
        // 1.5 * 2^23: taken away from a value between -2^22 and 0 and added back, rounding towards zero, it leaves
        // the value's whole part.
        constexpr ee::Float kWholePartBias = std::bit_cast<float>(0x4B400000u);        // 12582912
        // sin(2 pi u) for u from -1/4 to 1/4, odd powers of u to the ninth.
        constexpr ee::Float kSineTerm1 = std::bit_cast<float>(0x40C90FDAu); // 6.2831850
        constexpr ee::Float kSineTerm3 = std::bit_cast<float>(0xC2255DE0u); // -41.341675
        constexpr ee::Float kSineTerm5 = std::bit_cast<float>(0x42A33457u); // 81.602226
        constexpr ee::Float kSineTerm7 = std::bit_cast<float>(0xC2992661u); // -76.574959
        constexpr ee::Float kSineTerm9 = std::bit_cast<float>(0x421ED7B7u); // 39.710659

        // One lane of the microprogram's cosine, and the steps it leaves in VU0's registers there.
        struct CosineOnVu0
        {
            ee::Float cosine;      // vf1
            ee::Float u8;          // vf2: u^8
            ee::Float u4;          // vf3: u^4
            ee::Float term9;       // vf4: u * kSineTerm9
            ee::Float term5;       // vf5: u * kSineTerm5
            ee::Float term7;       // vf6: u^3 * kSineTerm7
            ee::Float partialSum;  // ACC: the polynomial but its ninth-power term
        };

        // The microprogram's maths on one lane (VU0's arithmetic and flags there: `vu`): the cosine of `radians`.
        // Its distance into the turn (a fraction f of it) becomes u = |f - 1/2| - 1/4, from -1/4 to 1/4, whose
        // sin(2 pi u) is cos(2 pi f).
        CosineOnVu0 cosineOnVu0(ee::Float radians, ee::vu0::MicroLane &vu)
        {
            const ee::Float magnitude = ee::abs(radians);
            const ee::Float minusTurns = vu.mul(magnitude, kMinusTurnsARadian);
            const ee::Float minusWholeTurns = vu.add(vu.sub(minusTurns, kWholePartBias), kWholePartBias);
            const ee::Float turnFraction = vu.multiplySubtract(minusWholeTurns, magnitude, kMinusTurnsARadian);
            const ee::Float u = vu.sub(ee::abs(vu.sub(turnFraction, 0.5f)), 0.25f);

            const ee::Float u2 = vu.mul(u, u);
            ee::Float term7 = vu.mul(u, kSineTerm7);
            const ee::Float term3 = vu.mul(u, kSineTerm3);
            const ee::Float term5 = vu.mul(u, kSineTerm5);
            const ee::Float u4 = vu.mul(u2, u2);
            term7 = vu.mul(term7, u2);
            ee::Float sum = vu.mul(term3, u2);
            const ee::Float term9 = vu.mul(u, kSineTerm9);
            const ee::Float u8 = vu.mul(u4, u4);
            sum = vu.multiplyAdd(sum, term7, u4);
            sum = vu.multiplyAdd(sum, term5, u4);
            sum = vu.multiplyAdd(sum, u, kSineTerm1);
            return {vu.multiplyAdd(sum, term9, u8), u8, u4, term9, term5, term7, sum};
        }

        struct SineAndCosine
        {
            ee::Float sine;
            ee::Float cosine;
        };

        // The microprogram (VU0's address 0) on vf1 as angleSin and angleCos hand it over: x the angle, z and w the
        // upper half of its register (`carried`; y, the angle's sign, it sets itself). The sine (x: the cosine a
        // quarter turn back) and the cosine (y); z and w get the same of the upper half's low word as a float, less
        // a quarter turn and as it is.
        SineAndCosine sineAndCosineOnVu0(Angle angle, uint64_t carried)
        {
            ee::vu0::MicroLane lanes[4];
            const ee::Float carriedWord = ee::Float::fromBits(static_cast<uint32_t>(carried));
            const ee::Float radians = lanes[0].mul(ee::toFloat(angle), kAngleToRadians);
            const ee::Float turned[4] = {
                lanes[0].sub(radians, kQuarterTurnRadians),
                lanes[1].add(0.0f, radians),
                lanes[2].sub(carriedWord, kQuarterTurnRadians),
                lanes[3].mul(1.0f, carriedWord),
            };
            CosineOnVu0 lane[4];
            for (int i = 0; i < 4; ++i)
                lane[i] = cosineOnVu0(turned[i], lanes[i]);

            ee::vu0::Leave vu0;
            const auto across = [&lane](ee::Float CosineOnVu0::*step)
            {
                return ee::vu0::Lanes{lane[0].*step, lane[1].*step, lane[2].*step, lane[3].*step};
            };
            vu0.vf(1, across(&CosineOnVu0::cosine));
            vu0.vf(2, across(&CosineOnVu0::u8));
            vu0.vf(3, across(&CosineOnVu0::u4));
            vu0.vf(4, across(&CosineOnVu0::term9));
            vu0.vf(5, across(&CosineOnVu0::term5));
            vu0.vf(6, across(&CosineOnVu0::term7));
            vu0.acc(across(&CosineOnVu0::partialSum));
            vu0.microFlags(lanes);
            return {lane[0].cosine, lane[1].cosine};
        }
    }

    // sineFromVu0, cosineFromVu0: ZGE4's angleSin and angleCos, the microprogram's x and y.
    ee::Float sineFromVu0(Angle angle) { return sineAndCosineOnVu0(angle, ee::vu0::firstArgumentUpperHalf()).sine; }
    ee::Float cosineFromVu0(Angle angle) { return sineAndCosineOnVu0(angle, ee::vu0::firstArgumentUpperHalf()).cosine; }

    ee::Float angleSin(Angle angle)
    {
        return sinf(ee::toFloat(angle) * kAngleToRadians);
    }
    READABLE_FUNCTION_VU0_OWN(angleSin, Verified, dm2, sineFromTable, bmx, sineFromVu0);

    namespace // the game functions below call: ee/plain.h
    {
        ee::Float cosf(ee::Float x) { return ee::game<ee::Float>(kCosf, x); }
    }

    ee::Float angleCos(Angle angle)
    {
        return cosf(ee::toFloat(angle) * kAngleToRadians);
    }
    READABLE_FUNCTION_VU0_OWN(angleCos, Verified, dm2, cosineFromTable, bmx, cosineFromVu0);

    namespace // the game functions below call: ee/plain.h
    {
        ee::Float acosf(ee::Float x) { return ee::game<ee::Float>(kAcosf, x); }
    }

    Angle angleAcos(ee::Float x)
    {
        return radiansToAngle(acosf(x));
    }
    READABLE_FUNCTION_OWN(angleAcos, Verified, dm2, arcCosineFromTable);

    namespace // the game functions below call: ee/plain.h
    {
        ee::Float atan2f(ee::Float y, ee::Float x) { return ee::game<ee::Float>(kAtan2f, y, x); }
    }

    Angle angleAtan2(ee::Float y, ee::Float x)
    {
        return radiansToAngle(atan2f(y, x));
    }
    READABLE_FUNCTION_OWN(angleAtan2, Verified, dm2, arcTangent2FromTable);

    ee::Float floorOf(ee::Float x)
    {
        return sce::libm::floorf(x);
    }
    READABLE_FUNCTION(floorOf, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        ee::Float ceilf(ee::Float x) { return ee::game<ee::Float>(kCeilf, x); }
    }

    ee::Float ceilingOf(ee::Float x)
    {
        return ceilf(x);
    }
    READABLE_FUNCTION(ceilingOf, Verified);

    Angle angleAsin(ee::Float x)
    {
        return radiansToAngle(sce::libm::asinf(x));
    }
    READABLE_FUNCTION_OWN(angleAsin, Verified, dm2, arcSineFromTable);

    namespace // the game functions below call: ee/plain.h
    {
        ee::Float tanf(ee::Float x) { return ee::game<ee::Float>(kTanf, x); }
    }

    ee::Float angleTan(Angle angle)
    {
        return tanf(ee::toFloat(angle) * kAngleToRadians);
    }
    READABLE_FUNCTION_OWN(angleTan, Verified, dm2, tangentFromTables);

    Angle angleAtan(ee::Float x)
    {
        return radiansToAngle(sce::libm::atanf(x));
    }
    READABLE_FUNCTION_OWN(angleAtan, Verified, dm2, arcTangentFromTable);
}
