// VU1's pipelines and results, out of line: the parts of the step (vu1_step.h) too big or too rare to inline into
// every pair. Compiled into the runtime for its interpreter and into every game module with ahead-of-time compiled
// microprograms (vu1/vu1_core.h): freestanding, against the core alone.
#include "vu1/vu1_core.h"
#include "vu1_detail.h"
#include "vu1_exec_lower.h"
#include "vu1_exec_upper.h"
#include "vu1_step.h"

float VU1Core::broadcast(const float *vf, uint8_t bc)
{
    return normalizeOperand(vf[bc & 3u]);
}



float VU1Core::normalizeResult(float value, uint32_t &laneFlags) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    const uint32_t exponent = (bits >> 23) & 0xFFu;

    laneFlags = sign != 0u ? 0x2u : 0u;
    if (magnitude == 0u)
    {
        laneFlags |= 0x1u;
    }
    else if (exponent == 0u)
    {
        laneFlags |= 0x5u;
        bits = sign;
    }
    else if (exponent == 0xFFu)
    {
        laneFlags |= 0x8u;
        bits = sign | 0x7F7FFFFFu;
    }

    std::memcpy(&value, &bits, sizeof(value));
    return value;
}


void VU1Core::normalizeFmacResult(float *result, uint8_t dest,
                                         uint8_t laneFlags[4])
{
    for (uint32_t component = 0; component < 4u; ++component)
    {
        laneFlags[component] = 0u;
        if ((dest & laneForComponent(component)) == 0u)
            continue;

        if (m_fmacFastFlags)
        {
            // Operands in [2^-60, 2^61) (see fmacOperandsInSafeRange): a
            // non-zero result is a normal number with the sign of the exact
            // result; only zero results need the exact check.
            uint32_t bits = 0;
            std::memcpy(&bits, &result[component], sizeof(bits));
            const uint32_t exponent = bits & 0x7F800000u;
            if ((bits & 0x7FFFFFFFu) != 0u && exponent != 0u && exponent != 0x7F800000u)
            {
                laneFlags[component] = static_cast<uint8_t>((bits >> 30) & 0x2u);
                continue;
            }
        }

        double exactResult = 0.0;
        if (calculateFmacExactResult(component, exactResult))
        {
            laneFlags[component] = normalizeFmacExactResult(result[component], exactResult);
            continue;
        }

        uint32_t flags = 0u;
        result[component] = normalizeResult(result[component], flags);
        laneFlags[component] = static_cast<uint8_t>(flags);
    }
}

// The instruction's result in double, for its flags (normalizeFmacExactResult:
// zero, sign, overflow, underflow): a product of two floats is exact in it,
// a sum is rounded once. This is the arithmetic the reference hosts always
// did (on macOS arm64 and Windows long double is double); it was long double,
// which on Android's arm64 is software quad precision (~6 % of the VU1 stage
// on the Thor) and on x86-64 Linux the x87's 80 bits.
bool VU1Core::calculateFmacExactResult(uint32_t component,
                                               double &result) const
{
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu
                                ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu))
                                : 0xFFu;
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);

    const auto operand = [this](float value)
    {
        return static_cast<double>(normalizeOperand(value));
    };
    const auto vs = [&](uint32_t lane)
    {
        return operand(m_state.vf[fs][lane]);
    };
    const auto vt = [&](uint32_t lane)
    {
        return operand(m_state.vf[ft][lane]);
    };
    const auto acc = [&](uint32_t lane)
    {
        return operand(m_state.acc[lane]);
    };

    const double q = operand(m_state.q);
    const double i = operand(m_state.i);

    if (op < 0x3Cu)
    {
        if (op <= 0x03u)
            result = vs(component) + vt(op & 3u);
        else if (op <= 0x07u)
            result = vs(component) - vt(op & 3u);
        else if (op <= 0x0Bu)
            result = acc(component) + vs(component) * vt(op & 3u);
        else if (op <= 0x0Fu)
            result = acc(component) - vs(component) * vt(op & 3u);
        else if (op >= 0x18u && op <= 0x1Bu)
            result = vs(component) * vt(op & 3u);
        else
        {
            switch (op)
            {
            case 0x1Cu:
                result = vs(component) * q;
                break;
            case 0x1Eu:
                result = vs(component) * i;
                break;
            case 0x20u:
                result = vs(component) + q;
                break;
            case 0x21u:
                result = acc(component) + vs(component) * q;
                break;
            case 0x22u:
                result = vs(component) + i;
                break;
            case 0x23u:
                result = acc(component) + vs(component) * i;
                break;
            case 0x24u:
                result = vs(component) - q;
                break;
            case 0x25u:
                result = acc(component) - vs(component) * q;
                break;
            case 0x26u:
                result = vs(component) - i;
                break;
            case 0x27u:
                result = acc(component) - vs(component) * i;
                break;
            case 0x28u:
                result = vs(component) + vt(component);
                break;
            case 0x29u:
                result = acc(component) + vs(component) * vt(component);
                break;
            case 0x2Au:
                result = vs(component) * vt(component);
                break;
            case 0x2Cu:
                result = vs(component) - vt(component);
                break;
            case 0x2Du:
                result = acc(component) - vs(component) * vt(component);
                break;
            case 0x2Eu:
            {
                static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
                static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
                result = component == 3u
                             ? 0.0
                             : acc(component) - vs(left[component]) * vt(right[component]);
                break;
            }
            default:
                return false;
            }
        }
        return true;
    }

    if (special <= 0x03u)
        result = vs(component) + vt(special & 3u);
    else if (special <= 0x07u)
        result = vs(component) - vt(special & 3u);
    else if (special <= 0x0Bu)
        result = acc(component) + vs(component) * vt(special & 3u);
    else if (special <= 0x0Fu)
        result = acc(component) - vs(component) * vt(special & 3u);
    else if (special >= 0x18u && special <= 0x1Bu)
        result = vs(component) * vt(special & 3u);
    else
    {
        switch (special)
        {
        case 0x1Cu:
            result = vs(component) * q;
            break;
        case 0x1Eu:
            result = vs(component) * i;
            break;
        case 0x20u:
            result = vs(component) + q;
            break;
        case 0x21u:
            result = acc(component) + vs(component) * q;
            break;
        case 0x22u:
            result = vs(component) + i;
            break;
        case 0x23u:
            result = acc(component) + vs(component) * i;
            break;
        case 0x24u:
            result = vs(component) - q;
            break;
        case 0x25u:
            result = acc(component) - vs(component) * q;
            break;
        case 0x26u:
            result = vs(component) - i;
            break;
        case 0x27u:
            result = acc(component) - vs(component) * i;
            break;
        case 0x28u:
            result = vs(component) + vt(component);
            break;
        case 0x29u:
            result = acc(component) + vs(component) * vt(component);
            break;
        case 0x2Au:
            result = vs(component) * vt(component);
            break;
        case 0x2Cu:
            result = vs(component) - vt(component);
            break;
        case 0x2Du:
            result = acc(component) - vs(component) * vt(component);
            break;
        case 0x2Eu:
        {
            static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
            static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
            result = component == 3u
                         ? 0.0
                         : vs(left[component]) * vt(right[component]);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

// fmacSpecialized with operands in the safe range (fmacOperandsInSafeRange)
// and a zero or denormal result lane: what execUpper's exact path gives
// (normalizeFmacResult with fast flags, calculateFmacExactResult,
// normalizeFmacExactResult, updateFmacFlags) without running the instruction
// again. a and b are the operands per result lane (crossed for OPMULA and
// OPMSUB, the scalar for broadcast/Q/I forms).
bool VU1Core::finishFmacWithZeros(uint8_t kind, uint8_t dest, bool toAcc, uint8_t fd, const float *a,
                                         const float *b, const float *acc, const uint32_t *resultBits,
                                         uint32_t extraSticky)
{
    float values[4];
    std::memcpy(values, resultBits, sizeof(values));
    uint32_t mac = 0u;
    uint32_t status = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint8_t lane = laneForComponent(component);
        if ((dest & lane) == 0u)
            continue;
        uint8_t flags = static_cast<uint8_t>((resultBits[component] >> 30) & 0x2u);
        if ((resultBits[component] & 0x7F800000u) == 0u)
        {
            const double x = a[component];
            const double y = b[component];
            double exact = 0.0;
            switch (kind)
            {
            case FmacPlan::Add:
                exact = x + y;
                break;
            case FmacPlan::Sub:
                exact = x - y;
                break;
            case FmacPlan::Mul:
            case FmacPlan::Opmula:
                exact = x * y;
                break;
            case FmacPlan::Madd:
                exact = static_cast<double>(acc[component]) + x * y;
                break;
            default: // Msub, Opmsub
                exact = static_cast<double>(acc[component]) - x * y;
                break;
            }
            if ((kind == FmacPlan::Opmula || kind == FmacPlan::Opmsub) && component == 3u)
                exact = 0.0;
            flags = normalizeFmacExactResult(values[component], exact);
        }
        if ((flags & 0x1u) != 0u)
            mac |= lane;
        if ((flags & 0x2u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 4;
        if ((flags & 0x4u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 8;
        if ((flags & 0x8u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 12;
        status |= flags;
    }
    applyDest(toAcc ? m_state.acc : m_state.vf[fd], values, dest);

    queueMacStatus(mac, status, extraSticky);
    return true;
}

uint8_t VU1Core::normalizeFmacExactResult(float &value,
                                                  double exactResult) const
{
    const bool negative = std::signbit(exactResult);
    const double magnitude = std::fabs(exactResult);
    const double maximum = static_cast<double>(std::numeric_limits<float>::max());
    const double minimum = static_cast<double>(std::numeric_limits<float>::min());
    uint8_t flags = negative ? 0x2u : 0u;

    uint32_t bits = negative ? 0x80000000u : 0u;
    if (magnitude == 0.0)
    {
        flags |= 0x1u;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude > maximum)
    {
        flags |= 0x8u;
        bits |= 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude < minimum)
    {
        flags |= 0x5u;
        std::memcpy(&value, &bits, sizeof(value));
    }

    return flags;
}

uint32_t VU1Core::calculateFmacProductSticky(uint8_t dest) const
{
    uint32_t extraSticky = 0u;
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : 0xFFu;
    const bool productSum =
        (op >= 0x08u && op <= 0x0Fu) ||
        op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
        op == 0x29u || op == 0x2Du || op == 0x2Eu ||
        (special >= 0x08u && special <= 0x0Fu) ||
        special == 0x21u || special == 0x23u || special == 0x25u ||
        special == 0x27u || special == 0x29u || special == 0x2Du;
    if (!productSum)
        return 0u;

    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & laneForComponent(component)) == 0u)
            continue;
        static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
        const uint8_t leftComponent = op == 0x2Eu ? crossLeft[component] : static_cast<uint8_t>(component);
        const float left = normalizeOperand(m_state.vf[fs][leftComponent]);
        float right = 0.0f;
        if ((op >= 0x08u && op <= 0x0Fu) || (special >= 0x08u && special <= 0x0Fu))
        {
            right = normalizeOperand(m_state.vf[ft][(op >= 0x08u && op <= 0x0Fu ? op : special) & 3u]);
        }
        else if (op == 0x21u || op == 0x25u || special == 0x21u || special == 0x25u)
        {
            right = normalizeOperand(m_state.q);
        }
        else if (op == 0x23u || op == 0x27u || special == 0x23u || special == 0x27u)
        {
            right = normalizeOperand(m_state.i);
        }
        else if (op == 0x2Eu)
        {
            right = normalizeOperand(m_state.vf[ft][crossRight[component]]);
        }
        else
        {
            right = normalizeOperand(m_state.vf[ft][component]);
        }

        uint8_t productFlags = 0u;
        if (m_fmacFastFlags)
        {
            // Factors in range: the product cannot overflow or underflow.
            uint32_t leftBits = 0, rightBits = 0;
            std::memcpy(&leftBits, &left, sizeof(leftBits));
            std::memcpy(&rightBits, &right, sizeof(rightBits));
            productFlags = static_cast<uint8_t>((((leftBits ^ rightBits) >> 30) & 0x2u) |
                                                ((leftBits & 0x7FFFFFFFu) == 0u || (rightBits & 0x7FFFFFFFu) == 0u ? 0x1u : 0u));
        }
        else
        {
            float product = left * right;
            const double exactProduct = static_cast<double>(left) * static_cast<double>(right);
            productFlags = normalizeFmacExactResult(product, exactProduct);
        }
        // Product-sum instructions report Z/S/U/O from the add/subtract result
        // as current flags, while every product condition accumulates into the
        // corresponding sticky flag.
        extraSticky |= productFlags & 0xFu;
    }
    return extraSticky;
}

void VU1Core::updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest,
                                     uint32_t extraSticky)
{
    if (dest == 0u)
        return;

    uint32_t mac = 0u;
    uint32_t status = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint8_t lane = laneForComponent(component);
        if ((dest & lane) == 0u)
            continue;

        const uint32_t flags = laneFlags[component];
        if ((flags & 0x1u) != 0u)
            mac |= lane;
        if ((flags & 0x2u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 4;
        if ((flags & 0x4u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 8;
        if ((flags & 0x8u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 12;
        status |= flags;
    }

    queueMacStatus(mac, status, extraSticky);
}


void VU1Core::suppressSameCycleFlags(bool status, bool clip)
{
    // FSSET drops the status (not MAC) results, FCSET the clip results
    // issued in its cycle.
    if (status)
    {
        for (uint32_t index = 0; index < m_macFlags.count; ++index)
        {
            FlagPipelineEntry &entry = m_macFlags.entries[(m_macFlags.head + index) % kMaxFlagEntries];
            if (entry.issueCycle == m_cycle && (entry.writes & FlagWritesMacStatus) &&
                !(entry.writes & FlagStatusDropped))
            {
                entry.writes |= FlagStatusDropped; // MAC only
                ++m_macFlags.special;
            }
        }
    }
    if (clip)
    {
        for (uint32_t index = 0; index < m_clipFlags.count; ++index)
        {
            FlagPipelineEntry &entry = m_clipFlags.entries[(m_clipFlags.head + index) % kMaxFlagEntries];
            if (entry.issueCycle == m_cycle)
                entry.writes = 0u;
        }
    }
}

void VU1Core::applyFmacDest(float *dst, float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDest(dst, result, dest);
}

void VU1Core::applyFmacDestAcc(float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDestAcc(result, dest);
}

void VU1Core::queueFsset(uint16_t immediate)
{
    suppressSameCycleFlags(true, false);
    if (pushFlag(m_macFlags, static_cast<uint32_t>(immediate) & 0xFC0u, FlagWritesSticky))
    {
        // Sticky bits from here on are FSSET's plus what follows.
        ++m_macFlags.special;
        m_macFlags.prefix = 0u;
        return;
    }
    reportReservedInstruction(false, 0xFFFFFFFEu);
}

void VU1Core::queueClip(uint32_t clip)
{
    m_workingClip = ((m_workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
    if (pushFlag(m_clipFlags, m_workingClip, FlagWritesClip))
        return;
    reportReservedInstruction(true, 0xFFFFFFFDu);
}

void VU1Core::queueFcset(uint32_t clip)
{
    m_workingClip = clip & 0xFFFFFFu;
    suppressSameCycleFlags(false, true);
    if (pushFlag(m_clipFlags, m_workingClip, FlagWritesClip))
        return;
    reportReservedInstruction(false, 0xFFFFFFFAu);
}

void VU1Core::queueQ(float value, uint32_t latency, uint32_t statusDi)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    m_fdiv.valid = true;
    m_fdiv.readyCycle = m_cycle + latency;
    noteCommitCycle(m_fdiv.readyCycle);
    m_fdiv.value = value;
    m_fdiv.statusDi = statusDi & 0x30u;
}

void VU1Core::queueP(float value, uint32_t latency)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!entry.valid)
        {
            entry.valid = true;
            entry.readyCycle = m_cycle + latency;
            noteCommitCycle(entry.readyCycle);
            entry.value = value;
            // EFU throughput is one cycle shorter than result visibility.
            m_efuResourceReady = m_cycle + (latency > 0u ? latency - 1u : 0u);
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF9u);
}



void VU1Core::logPairWrites(const DecodedInstructionPair &decoded, uint8_t writtenVi,
                                   const uint32_t (&oldBits)[4][4])
{
    auto push = [this](uint8_t kind, uint8_t reg, uint8_t lanes, uint32_t latency,
                       const uint32_t (&before)[4], const uint32_t (&after)[4])
    {
        InFlightWrite &entry = m_writeLog[m_writeLogNext++ % kWriteLogSize];
        entry.readyCycle = m_cycle + latency;
        entry.kind = kind;
        entry.reg = reg;
        entry.lanes = lanes;
        std::memcpy(entry.oldBits, before, sizeof(entry.oldBits));
        std::memcpy(entry.newBits, after, sizeof(entry.newBits));
    };
    uint32_t now[4]{};
    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    if (upperWrite.reg != 0u)
    {
        copyBits(now, m_state.vf[upperWrite.reg], 4u);
        push(0u, upperWrite.reg, upperWrite.lanes,
             decoded.upperUsage.vfLatency != 0u ? decoded.upperUsage.vfLatency : decoded.upperUsage.latency,
             oldBits[0], now);
    }
    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    if (lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg && lowerWrite.reg != upperWrite.reg)
    {
        copyBits(now, m_state.vf[lowerWrite.reg], 4u);
        push(0u, lowerWrite.reg, lowerWrite.lanes,
             decoded.lowerUsage.vfLatency != 0u ? decoded.lowerUsage.vfLatency : decoded.lowerUsage.latency,
             oldBits[1], now);
    }
    if (writtenVi != 0u)
    {
        copyBits(now, &m_state.vi[writtenVi], 1u);
        push(1u, writtenVi, 1u,
             decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency,
             oldBits[2], now);
    }
    if (decoded.upperUsage.accWrite != 0u)
    {
        copyBits(now, m_state.acc, 4u);
        push(2u, 0u, decoded.upperUsage.accWrite, kAccForwardLatency, oldBits[3], now);
    }
}

void VU1Core::supersedeDeferredWrites(const DecodedInstructionPair &decoded, uint8_t writtenVi)
{
    // A newer write to the same lane wins over a deferred older one, whatever
    // their ready cycles (the hardware keeps the latest result).
    for (uint32_t index = 0; index < m_deferredWriteCount; ++index)
    {
        InFlightWrite &entry = m_deferredWrites[index];
        if (entry.kind == 0u)
        {
            const VfAccess upperWrite = decoded.upperUsage.vfWrite;
            const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
            if (upperWrite.reg == entry.reg)
                entry.lanes &= static_cast<uint8_t>(~upperWrite.lanes);
            if (lowerWrite.reg == entry.reg && decoded.suppressedLowerVf != lowerWrite.reg)
                entry.lanes &= static_cast<uint8_t>(~lowerWrite.lanes);
        }
        else if (entry.kind == 1u)
        {
            if (writtenVi == entry.reg)
                entry.lanes = 0u;
        }
        else
        {
            entry.lanes &= static_cast<uint8_t>(~decoded.upperUsage.accWrite);
        }
    }
}

void VU1Core::commitDeferredWrites()
{
    size_t kept = 0;
    for (uint32_t index = 0; index < m_deferredWriteCount; ++index)
    {
        const InFlightWrite &entry = m_deferredWrites[index];
        if (entry.readyCycle > m_cycle)
        {
            m_deferredWrites[kept++] = entry;
            continue;
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if (entry.kind == 1u)
            {
                if (component == 0u && entry.lanes != 0u)
                    std::memcpy(&m_state.vi[entry.reg], &entry.newBits[0], sizeof(uint32_t));
                continue;
            }
            if ((entry.lanes & laneForComponent(component)) == 0u)
                continue;
            float *target = entry.kind == 0u ? &m_state.vf[entry.reg][component] : &m_state.acc[component];
            std::memcpy(target, &entry.newBits[component], sizeof(uint32_t));
        }
    }
    m_deferredWriteCount = kept;
}

void VU1Core::applyReadyFlagsSlow()
{
    constexpr uint32_t kMask = 63u;
    static_assert(sizeof(FlagRing::entries) / sizeof(FlagPipelineEntry) == kMask + 1u);
    if (m_macFlags.ready(m_cycle))
    {
        // Ready entries are a prefix of the ring (issue order, one latency).
        FlagRing &ring = m_macFlags;
        uint32_t ready = ring.count;
        while (ring.entries[(ring.head + ready - 1u) & kMask].issueCycle + kFmacLatency > m_cycle)
            --ready;
        if (ring.special == 0u && !ring.inOrder)
        {
            // FMAC results only: the last one's MAC and current bits, and
            // every sticky contribution so far (already applied ones again,
            // which changes nothing).
            const FlagPipelineEntry &last = ring.entries[(ring.head + ready - 1u) & kMask];
            m_state.mac = last.value & 0xFFFFu;
            m_state.status = (m_state.status & 0xFF0u) | ((last.value >> 16) & 0xFu) |
                             (((last.writes >> FlagPrefixShift) & 0xFu) << 6);
            ring.head = (ring.head + ready) & kMask;
            ring.count -= ready;
        }
        else
            applyMacFlagsInOrder(ready);
        if (ring.count == 0u)
        {
            ring.prefix = 0u;
            ring.inOrder = false;
        }
    }
    if (m_clipFlags.ready(m_cycle))
    {
        FlagRing &ring = m_clipFlags;
        uint32_t ready = ring.count;
        while (ring.entries[(ring.head + ready - 1u) & kMask].issueCycle + kFmacLatency > m_cycle)
            --ready;
        // The last ready entry that writes (FCSET drops same-cycle CLIPs).
        for (uint32_t index = ready; index-- > 0u;)
        {
            const FlagPipelineEntry &entry = ring.entries[(ring.head + index) & kMask];
            if (entry.writes & FlagWritesClip)
            {
                m_state.clip = entry.value;
                break;
            }
        }
        ring.head = (ring.head + ready) & kMask;
        ring.count -= ready;
    }
}

void VU1Core::applyMacFlagsInOrder(uint32_t count)
{
    FlagRing &ring = m_macFlags;
    for (uint32_t index = 0; index < count; ++index)
    {
        const FlagPipelineEntry &entry = ring.entries[ring.head];
        ring.head = (ring.head + 1u) % kMaxFlagEntries;
        --ring.count;
        if (entry.writes & (FlagWritesSticky | FlagStatusDropped))
            --ring.special;
        if (entry.writes & FlagWritesMacStatus)
        {
            m_state.mac = entry.value & 0xFFFFu;
            if (!(entry.writes & FlagStatusDropped))
            {
                const uint32_t current = (entry.value >> 16) & 0xFu;
                m_state.status = (m_state.status & 0xFF0u) | current | ((current | ((entry.value >> 20) & 0xFu)) << 6);
            }
        }
        else if (entry.writes & FlagWritesSticky)
            m_state.status = (m_state.status & 0x03Fu) | (entry.value & 0xFC0u);
    }
}

void VU1Core::commitPipelinesSlow()
{
    if (m_deferredWriteCount != 0u)
        commitDeferredWrites();
    // Flags first: Q's D/I bits go into the same status register.
    applyReadyFlags();

    if (m_fdiv.valid && m_fdiv.readyCycle <= m_cycle)
    {
        m_state.q = m_fdiv.value;
        const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
        m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
        m_fdiv = {};
    }

    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (entry.valid && entry.readyCycle <= m_cycle)
        {
            m_state.p = entry.value;
            entry = {};
        }
    }

    // Earliest cycle at which something is left to commit.
    uint64_t next = ~0ull;
    if (m_fdiv.valid)
        next = std::min(next, m_fdiv.readyCycle);
    for (const ScalarPipelineEntry &entry : m_efu)
        if (entry.valid)
            next = std::min(next, entry.readyCycle);
    for (uint32_t index = 0; index < m_deferredWriteCount; ++index)
        next = std::min(next, m_deferredWrites[index].readyCycle);
    m_nextCommitCycle = next;
}

void VU1Core::progressXgkick(uint32_t cycles)
{
    if (!m_xgkick.active || !m_activeVuData || m_activeVuDataSize == 0u)
        return;

    m_xgkick.cycleCredit += cycles;
    while (m_xgkick.active && m_xgkick.cycleCredit >= 2u)
    {
        if (m_xgkick.currentTagEnd > m_xgkick.copiedBytes + 16u && m_xgkick.cycleCredit >= 4u)
        {
            // Inside a GIFtag's data: the qwords the credit covers, up to the
            // tag's end and the end of data memory, in one copy (the tag was
            // checked to fit the packet buffer).
            const uint32_t source = (m_xgkick.sourceAddress + m_xgkick.copiedBytes) % m_activeVuDataSize;
            const uint32_t qwords = std::min(std::min(m_xgkick.cycleCredit / 2u, (m_xgkick.currentTagEnd - m_xgkick.copiedBytes) / 16u - 1u),
                                             (m_activeVuDataSize - source) / 16u);
            if (qwords > 0u)
            {
                std::memcpy(m_xgkickPacket + m_xgkick.copiedBytes, m_activeVuData + source, qwords * 16u);
                m_xgkick.copiedBytes += qwords * 16u;
                m_xgkick.cycleCredit -= qwords * 2u;
                continue;
            }
        }
        m_xgkick.cycleCredit -= 2u;
        if (m_xgkick.copiedBytes > XgkickPipeline::kBufferSize - 16u)
        {
            reportReservedInstruction(false, 0xFFFFFFFBu);
            m_xgkick.active = false;
            return;
        }

        const uint32_t qwordOffset = m_xgkick.copiedBytes;
        // Source and size are qword multiples, so a qword never wraps.
        const uint32_t source = (m_xgkick.sourceAddress + m_xgkick.copiedBytes) % m_activeVuDataSize;
        std::memcpy(m_xgkickPacket + qwordOffset, m_activeVuData + source, 16u);
        m_xgkick.copiedBytes += 16u;

        if (m_xgkick.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_xgkickPacket + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == 2u)
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
            {
                reportReservedInstruction(false, 0xFFFFFFF8u);
                m_xgkick.active = false;
                return;
            }

            if (tagBytes > XgkickPipeline::kBufferSize - qwordOffset)
            {
                reportReservedInstruction(false, 0xFFFFFFFBu);
                m_xgkick.active = false;
                return;
            }
            m_xgkick.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            m_xgkick.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (m_xgkick.currentTagEop)
                m_xgkick.totalBytes = m_xgkick.currentTagEnd;
        }

        if (m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            if (m_xgkick.currentTagEop)
                finishXgkick();
            else
            {
                // The next transferred qword is another GIFtag.
                m_xgkick.currentTagEnd = 0u;
                m_xgkick.currentTagEop = false;
            }
        }
    }
}

void VU1Core::finishXgkick()
{
    if (!m_xgkick.active)
        return;

    ++m_state.xgkickPackets;
    m_host.packet(m_host.context, m_xgkickPacket, m_xgkick.totalBytes);
    m_xgkick.active = false;
}

void VU1Core::startXgkick(uint32_t qwordAddress)
{
    if (m_unit != Unit::VU1 || !m_activeVuData || m_activeVuDataSize < 16u)
        return;

    const uint32_t sourceAddress = (qwordAddress * 16u) % m_activeVuDataSize;
    // The packet buffer is filled before it is read: reset only the state.
    m_xgkick.totalBytes = 0;
    m_xgkick.copiedBytes = 0;
    m_xgkick.currentTagEnd = 0;
    m_xgkick.currentTagEop = false;
    m_xgkick.active = true;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;
    m_xgkickSynced = m_cycle;
}


void VU1Core::advanceTo(uint64_t targetCycle)
{
    while (m_cycle < targetCycle)
    {
        // Nothing happens before the next commit (PATH1 runs lazily).
        const uint64_t next = std::min(targetCycle, m_nextCommitCycle);
        if (next > m_cycle + 1u)
            m_cycle = next - 1u;
        advanceOneCycle();
    }
}

void VU1Core::aotBlockBail(const DecodedInstructionPair *pairs, const AotMark *marks, uint32_t markCount,
                                  uint32_t executed, uint32_t pc, uint64_t done, uint64_t next)
{
    if (next > done)
        aotSyncTo(done, next);
    else
        m_cycle = next;
    // In issue order: a later write of a lane replaces the earlier one.
    for (uint32_t index = 0; index < markCount; ++index)
        aotMarkWrites(pairs[marks[index].index], marks[index].cycle);
    m_pairsExecuted += executed;
    m_seqPairs = 0;
    m_expectedPc = ~0u;
    m_state.pc = pc;
}

void VU1Core::execUpperOutOfLine(uint32_t instr)
{
    execUpper(instr);
}

void VU1Core::aotStepPairSlow(const DecodedInstructionPair &decoded, RunContext &rc)
{
    stepPair(decoded, true, rc);
}

bool VU1Core::pipelinesPending() const
{
    if (m_fdiv.valid || m_xgkick.active || m_cycle < m_writesDoneCycle)
        return true;
    for (const ScalarPipelineEntry &entry : m_efu)
        if (entry.valid)
            return true;
    if (m_macFlags.count != 0u || m_clipFlags.count != 0u)
        return true;
    return false;
}

