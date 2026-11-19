#include "runtime/gs/gs_record.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_types.h"
#include "runtime/ps2_memory.h"

#include "runtime/gs/gs_native.h"
#include <cstring>
#include <iostream>
#include <mutex>
#include <unordered_map>

namespace
{
    constexpr uint8_t kRegPrim = 0x00;
    constexpr uint8_t kRegRgbaq = 0x01;
    constexpr uint8_t kRegSt = 0x02;
    constexpr uint8_t kRegUv = 0x03;
    constexpr uint8_t kRegTex0_1 = 0x06;
    constexpr uint8_t kRegTex0_2 = 0x07;
    constexpr uint8_t kRegFog = 0x0A;
    constexpr uint8_t kRegTex2_1 = 0x16;
    constexpr uint8_t kRegTex2_2 = 0x17;
    constexpr uint8_t kRegPrmodecont = 0x1A;
    constexpr uint8_t kRegPrmode = 0x1B;
    constexpr uint8_t kRegTexflush = 0x3F;
    constexpr uint8_t kRegLastContextReg = 0x4F;
    constexpr uint8_t kRegBitbltbuf = 0x50;
    constexpr uint8_t kRegTrxpos = 0x51;
    constexpr uint8_t kRegTrxreg = 0x52;

    // Register writes that have side effects beyond latching state (vertex
    // kicks, transfers, flushes) are not part of the restored state.
    bool isPlainStateRegister(uint8_t reg)
    {
        switch (reg)
        {
        case 0x04: // XYZF2
        case 0x05: // XYZ2
        case 0x0C: // XYZF3
        case 0x0D: // XYZ3
        case kRegTexflush:
            return false;
        default:
            return reg <= kRegLastContextReg;
        }
    }

    bool writeAll(std::FILE *file, const void *data, size_t bytes)
    {
        return bytes == 0u || std::fwrite(data, 1, bytes, file) == bytes;
    }

    bool readAll(std::FILE *file, void *data, size_t bytes)
    {
        return bytes == 0u || std::fread(data, 1, bytes, file) == bytes;
    }

    void privToArray(const GSRegisters *priv, uint64_t out[8])
    {
        std::memset(out, 0, sizeof(uint64_t) * 8u);
        if (!priv)
            return;
        out[0] = priv->pmode;
        out[1] = priv->smode2;
        out[2] = priv->dispfb1;
        out[3] = priv->display1;
        out[4] = priv->dispfb2;
        out[5] = priv->display2;
        out[6] = priv->bgcolor;
        out[7] = priv->vsyncTick.load(std::memory_order_relaxed);
    }

    void arrayToPriv(const uint64_t in[8], GSRegisters &priv)
    {
        priv.pmode = in[0];
        priv.smode2 = in[1];
        priv.dispfb1 = in[2];
        priv.display1 = in[3];
        priv.dispfb2 = in[4];
        priv.display2 = in[5];
        priv.bgcolor = in[6];
        priv.vsyncTick.store(in[7], std::memory_order_relaxed);
    }
}

namespace GSRecord
{
    namespace
    {
        constexpr uint32_t kNativeVersion = 2u; // 2: the draw's motion (frame interpolation); 1 read too

        struct Writer
        {
            std::vector<uint8_t> out;
            template <class T>
            void put(const T &v)
            {
                const uint8_t *p = reinterpret_cast<const uint8_t *>(&v);
                out.insert(out.end(), p, p + sizeof(T));
            }
            void bytes(const void *p, size_t n)
            {
                const uint8_t *b = static_cast<const uint8_t *>(p);
                out.insert(out.end(), b, b + n);
            }
        };

        struct ReadCursor
        {
            const uint8_t *p = nullptr;
            size_t left = 0;
            bool ok = true;
            template <class T>
            T get()
            {
                T v{};
                bytes(&v, sizeof(T));
                return v;
            }
            void bytes(void *dst, size_t n)
            {
                if (!ok || left < n)
                {
                    ok = false;
                    return;
                }
                std::memcpy(dst, p, n);
                p += n;
                left -= n;
            }
        };
    }

    std::vector<uint8_t> encodeNativeMesh(const GSNativeMesh &mesh)
    {
        Writer w;
        w.put(kNativeVersion);
        w.put(mesh.id);
        w.put(mesh.source);
        w.put(static_cast<uint32_t>(mesh.vertices.size()));
        w.put(static_cast<uint32_t>(mesh.skin.size()));
        w.bytes(mesh.boundsMin, sizeof(mesh.boundsMin));
        w.bytes(mesh.boundsMax, sizeof(mesh.boundsMax));
        w.bytes(mesh.vertices.data(), mesh.vertices.size() * sizeof(GSNativeVertex));
        w.bytes(mesh.skin.data(), mesh.skin.size() * sizeof(GSNativeSkinWeights));
        return std::move(w.out);
    }

    std::shared_ptr<GSNativeMesh> decodeNativeMesh(const uint8_t *data, uint32_t size)
    {
        ReadCursor r{data, size};
        if (const uint32_t version = r.get<uint32_t>(); version != 1u && version != kNativeVersion)
            return nullptr; // (a mesh is the same in both)
        auto mesh = std::make_shared<GSNativeMesh>();
        mesh->id = r.get<uint64_t>();
        mesh->source = r.get<uint32_t>();
        const uint32_t vertices = r.get<uint32_t>(), skin = r.get<uint32_t>();
        if (!r.ok || vertices > r.left / sizeof(GSNativeVertex) || (skin != 0u && skin != vertices))
            return nullptr;
        r.bytes(mesh->boundsMin, sizeof(mesh->boundsMin));
        r.bytes(mesh->boundsMax, sizeof(mesh->boundsMax));
        mesh->vertices.resize(vertices);
        r.bytes(mesh->vertices.data(), vertices * sizeof(GSNativeVertex));
        mesh->skin.resize(skin);
        r.bytes(mesh->skin.data(), skin * sizeof(GSNativeSkinWeights));
        return r.ok ? mesh : nullptr;
    }

    std::vector<uint8_t> encodeNativeDraw(const GSNativeDraw &d)
    {
        Writer w;
        w.put(kNativeVersion);
        w.put(d.mesh ? d.mesh->id : uint64_t(0));
        w.put(d.first);
        w.put(d.count);
        w.bytes(d.transform, sizeof(d.transform));
        w.put(d.nearW);
        w.put(d.normalOffset);
        w.put(static_cast<int32_t>(d.alpha));
        w.bytes(d.uv, sizeof(d.uv));
        w.bytes(d.clipTransform, sizeof(d.clipTransform));
        const uint8_t flags[8] = {static_cast<uint8_t>(d.clip), d.perspectiveColour, d.uvTransform, d.uvFromNormal,
                                  d.dropFlags, d.clampXY, d.affineUV,
                                  static_cast<uint8_t>((d.skin ? 1u : 0u) | (d.lights ? 2u : 0u) |
                                                       (d.motion.key ? 4u : 0u))};
        w.bytes(flags, sizeof(flags));
        if (d.skin)
        {
            // bit 0 lit, bit 1 the Xbox lighting's colour cap (128; else 255)
            w.put(static_cast<uint8_t>((d.skin->lit ? 1u : 0u) | (d.skin->maxColour < 255.0f ? 2u : 0u) |
                                       (d.skin->alphaFromLights ? 4u : 0u)));
            w.bytes(d.skin->lightDir, sizeof(d.skin->lightDir));
            w.bytes(d.skin->lightColour, sizeof(d.skin->lightColour));
            w.bytes(d.skin->palette, sizeof(d.skin->palette));
        }
        if (d.lights)
        {
            w.bytes(d.lights->lightDir, sizeof(d.lights->lightDir));
            w.bytes(d.lights->lightColour, sizeof(d.lights->lightColour));
            w.put(d.lights->scale);
            w.put(d.lights->alpha);
        }
        if (d.motion.key)
        {
            w.put(d.motion.key);
            w.bytes(d.motion.world, sizeof(d.motion.world));
            w.bytes(d.motion.camera, sizeof(d.motion.camera));
        }
        return std::move(w.out);
    }

    bool decodeNativeDraw(const uint8_t *data, uint32_t size, GSNativeDraw &d, uint64_t &meshId)
    {
        ReadCursor r{data, size};
        const uint32_t version = r.get<uint32_t>();
        if (version != 1u && version != kNativeVersion)
            return false;
        meshId = r.get<uint64_t>();
        d.first = r.get<uint32_t>();
        d.count = r.get<uint32_t>();
        r.bytes(d.transform, sizeof(d.transform));
        d.nearW = r.get<float>();
        d.normalOffset = r.get<float>();
        d.alpha = r.get<int32_t>();
        r.bytes(d.uv, sizeof(d.uv));
        r.bytes(d.clipTransform, sizeof(d.clipTransform));
        uint8_t flags[8] = {};
        r.bytes(flags, sizeof(flags));
        d.clip = static_cast<GSNativeClip>(flags[0]);
        d.perspectiveColour = flags[1] != 0u;
        d.uvTransform = flags[2] != 0u;
        d.uvFromNormal = flags[3] != 0u;
        d.dropFlags = flags[4];
        d.clampXY = flags[5] != 0u;
        d.affineUV = flags[6] != 0u;
        if (flags[7] & 1u)
        {
            auto skin = std::make_shared<GSNativeSkinning>();
            const uint8_t lit = r.get<uint8_t>();
            skin->lit = (lit & 1u) != 0u;
            skin->maxColour = (lit & 2u) != 0u ? 128.0f : 255.0f;
            skin->alphaFromLights = (lit & 4u) != 0u;
            r.bytes(skin->lightDir, sizeof(skin->lightDir));
            r.bytes(skin->lightColour, sizeof(skin->lightColour));
            r.bytes(skin->palette, sizeof(skin->palette));
            d.skin = std::move(skin);
        }
        if (flags[7] & 2u)
        {
            auto lights = std::make_shared<GSNativeLights>();
            r.bytes(lights->lightDir, sizeof(lights->lightDir));
            r.bytes(lights->lightColour, sizeof(lights->lightColour));
            lights->scale = r.get<float>();
            lights->alpha = r.get<uint8_t>();
            d.lights = std::move(lights);
        }
        if (version >= 2u && (flags[7] & 4u))
        {
            d.motion.key = r.get<uint64_t>();
            r.bytes(d.motion.world, sizeof(d.motion.world));
            r.bytes(d.motion.camera, sizeof(d.motion.camera));
        }
        return r.ok;
    }
}

bool GS::startRecording(const std::string &path, uint32_t vsyncCount)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (m_recordFile || !m_backend)
        return false;
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;

    std::vector<uint8_t> vram;
    m_backend->SnapshotVram(vram);
    std::vector<uint8_t> clut;
    m_backend->SaveClutState(clut);
    uint64_t priv[8];
    privToArray(m_privRegs, priv);

    const uint32_t vramSize = static_cast<uint32_t>(vram.size());
    const uint32_t clutSize = static_cast<uint32_t>(clut.size());
    bool ok = writeAll(file, GSRecord::kMagic, sizeof(GSRecord::kMagic)) &&
              writeAll(file, &vramSize, sizeof(vramSize)) &&
              writeAll(file, vram.data(), vram.size()) &&
              writeAll(file, m_shadowRegs.data(), sizeof(m_shadowRegs)) &&
              writeAll(file, m_shadowValid.data(), sizeof(m_shadowValid)) &&
              writeAll(file, &clutSize, sizeof(clutSize)) &&
              writeAll(file, clut.data(), clut.size()) &&
              writeAll(file, priv, sizeof(priv));
    if (!ok)
    {
        std::fclose(file);
        return false;
    }

    m_recordFile = file;
    m_recordVsyncsLeft = vsyncCount != 0u ? vsyncCount : 1u;
    m_recordedNativeMeshes.clear();
    std::cerr << "[gs:record] recording " << m_recordVsyncsLeft << " frame(s) to " << path << std::endl;
    return true;
}

void GS::stopRecording()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_recordFile)
        return;
    std::fclose(m_recordFile);
    m_recordFile = nullptr;
    m_recordVsyncsLeft = 0u;
    std::cerr << "[gs:record] stopped" << std::endl;
}

void GS::recordEventUnlocked(uint8_t type, const void *a, uint32_t aBytes, const void *b, uint32_t bBytes)
{
    if (!m_recordFile)
        return;
    const uint32_t size = aBytes + bBytes;
    const bool ok = writeAll(m_recordFile, &type, 1u) &&
                    writeAll(m_recordFile, &size, sizeof(size)) &&
                    writeAll(m_recordFile, a, aBytes) &&
                    writeAll(m_recordFile, b, bBytes);
    if (!ok)
    {
        std::fclose(m_recordFile);
        m_recordFile = nullptr;
    }
}

void GS::recordFrameMarkerUnlocked()
{
    uint64_t priv[8];
    privToArray(m_privRegs, priv);
    recordEventUnlocked(GSRecord::kFrameMarker, priv, sizeof(priv));
    if (m_recordVsyncsLeft > 0u && --m_recordVsyncsLeft == 0u)
        stopRecording();
}

namespace GSRecord
{
    Reader::~Reader()
    {
        if (m_file)
            std::fclose(m_file);
    }

    bool Reader::open(const std::string &path, GS &gs, uint8_t *vram, uint32_t vramSize, GSRegisters &privRegs)
    {
        m_file = std::fopen(path.c_str(), "rb");
        if (!m_file)
            return false;

        char magic[8];
        uint32_t recordedVramSize = 0u;
        if (!readAll(m_file, magic, sizeof(magic)) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0 ||
            !readAll(m_file, &recordedVramSize, sizeof(recordedVramSize)) || recordedVramSize != vramSize ||
            !readAll(m_file, vram, vramSize))
            return false;

        uint64_t shadow[256];
        uint8_t valid[256];
        uint32_t clutSize = 0u;
        if (!readAll(m_file, shadow, sizeof(shadow)) || !readAll(m_file, valid, sizeof(valid)) ||
            !readAll(m_file, &clutSize, sizeof(clutSize)) || clutSize > (1u << 20))
            return false;
        std::vector<uint8_t> clut(clutSize);
        uint64_t priv[8];
        if (!readAll(m_file, clut.data(), clut.size()) || !readAll(m_file, priv, sizeof(priv)))
            return false;

        arrayToPriv(priv, privRegs);

        auto restore = [&](uint8_t reg)
        {
            if (!valid[reg])
                return;
            uint64_t value = shadow[reg];
            // Never reload the CLUT from the restored TEX0/TEX2: the CLUT
            // buffer itself is restored below.
            if (reg == kRegTex0_1 || reg == kRegTex0_2 || reg == kRegTex2_1 || reg == kRegTex2_2)
                value &= ~(0x7ull << 61);
            gs.writeRegister(reg, value);
        };

        restore(kRegPrmodecont);
        restore(kRegPrmode);
        for (uint32_t reg = 0x06u; reg <= kRegLastContextReg; ++reg)
        {
            if (reg != kRegPrmodecont && reg != kRegPrmode && isPlainStateRegister(static_cast<uint8_t>(reg)))
                restore(static_cast<uint8_t>(reg));
        }
        restore(kRegBitbltbuf);
        restore(kRegTrxpos);
        restore(kRegTrxreg);
        restore(kRegRgbaq);
        restore(kRegSt);
        restore(kRegUv);
        restore(kRegFog);
        restore(kRegPrim);

        if (GSRasterBackend *backend = gs.rasterBackend())
            backend->RestoreClutState(clut);
        return true;
    }

    bool Reader::next(Event &out)
    {
        if (!m_file)
            return false;
        uint8_t type = 0u;
        uint32_t size = 0u;
        if (!readAll(m_file, &type, 1u) || !readAll(m_file, &size, sizeof(size)))
            return false;
        out.type = type;
        out.payload.resize(size);
        return readAll(m_file, out.payload.data(), size);
    }

    void applyFrameMarker(const Event &event, GSRegisters &privRegs)
    {
        if (event.type != kFrameMarker || event.payload.size() < sizeof(uint64_t) * 8u)
            return;
        uint64_t priv[8];
        std::memcpy(priv, event.payload.data(), sizeof(priv));
        arrayToPriv(priv, privRegs);
    }

    namespace
    {
        // Meshes of the traces replayed (by id; a trace's ids are unique).
        std::unordered_map<uint64_t, std::shared_ptr<const GSNativeMesh>> &replayMeshes()
        {
            static std::unordered_map<uint64_t, std::shared_ptr<const GSNativeMesh>> m;
            return m;
        }
        std::mutex &replayMeshesMutex()
        {
            static std::mutex m;
            return m;
        }
    }

    void replayEvent(const Event &event, GS &gs)
    {
        const uint8_t *data = event.payload.data();
        const uint32_t size = static_cast<uint32_t>(event.payload.size());
        switch (event.type)
        {
        case kGifPacket:
            gs.processGIFPacket(data, size);
            break;
        case kNativePackedGifPacket:
            gs.processNativePackedGIFPacket(data, size);
            break;
        case kUploadImageNative:
            if (size >= 32u)
            {
                uint64_t regs[4];
                std::memcpy(regs, data, sizeof(regs));
                gs.uploadImageNative(regs[0], regs[1], regs[2], regs[3], data + 32u, size - 32u);
            }
            break;
        case kWriteRegister:
            if (size >= 9u)
            {
                uint64_t value = 0u;
                std::memcpy(&value, data + 1, sizeof(value));
                gs.writeRegister(data[0], value);
            }
            break;
        case kClearFramebufferContext:
            if (size >= 8u)
            {
                uint32_t payload[2];
                std::memcpy(payload, data, sizeof(payload));
                gs.clearFramebufferContext(payload[0], payload[1]);
            }
            break;
        case kClearActiveFramebuffer:
            if (size >= 4u)
            {
                uint32_t rgba = 0u;
                std::memcpy(&rgba, data, sizeof(rgba));
                gs.clearActiveFramebuffer(rgba);
            }
            break;
        case kNativeMesh:
            if (auto mesh = decodeNativeMesh(data, size))
            {
                std::lock_guard<std::mutex> lock(replayMeshesMutex());
                replayMeshes()[mesh->id] = std::move(mesh);
            }
            break;
        case kFogPlanes:
            if (size >= 20u)
            {
                GSFogPlanes planes;
                int32_t count = 0;
                std::memcpy(&count, data, 4);
                planes.count = count;
                std::memcpy(&planes.nearest, data + 4, 4);
                std::memcpy(&planes.step, data + 8, 4);
                std::memcpy(&planes.zA, data + 12, 4);
                std::memcpy(&planes.zB, data + 16, 4);
                gs.noteFogPlanes(planes);
            }
            break;
        case kNativeDraw:
        {
            GSNativeDraw draw;
            uint64_t id = 0;
            if (!decodeNativeDraw(data, size, draw, id))
                break;
            {
                std::lock_guard<std::mutex> lock(replayMeshesMutex());
                auto it = replayMeshes().find(id);
                if (it == replayMeshes().end())
                    break;
                draw.mesh = it->second;
            }
            gs.drawNative(draw);
            break;
        }
        default:
            break;
        }
    }
}
