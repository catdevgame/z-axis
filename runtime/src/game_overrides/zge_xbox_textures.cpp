// The Xbox textures (zge_xbox_textures.h), the same in the three games.
//
// The Xbox releases have the same levels with the textures the PS2 builds
// cut down: Aggressive Inline's 4.6 times the texels in levels and 512x512
// skaters, DM2's and BMX XXX's levels 3.4 and 3.7 times (256x256 where the
// PS2 has 128x128), all DXT3 with full mip chains. The extras' setup
// (collection/extras/xbox_texture_pack.cpp; for AI also tools/xbox_pack.py
// in the bring-up workspace) pairs every PS2 texture with its Xbox picture
// (same file, same name, checked by picture) and writes a pack: one .xtp per
// PS2 file (runtime/ps2_texture_replace.h describes the format) under
// <pack>/textures/, named after the PS2 file's normalized path
// ("ASSETS/LEVELS/CIVIC/CIVIC.TXR.xtp"), keyed by what the GS will hold: the
// texture's size and the hash of its indices.
//
// Here: when the game opens a PS2 file the pack covers, a worker thread
// reads that .xtp and registers its pictures (one source per file; the file
// loaded last wins where two hold the same PS2 texture). The GPU renderer
// draws them in place of the PS2 textures. The game's RAM, its GS memory and
// its logic are untouched, so everything else (the CPU renderer, logic
// checks, saves) is as without the pack.
//
// Which texture a draw shows is told by the game itself, not guessed from GS
// memory: the texture cache's placeTextureLevel (zge/render/texture.cpp,
// called from the VIF1 interrupt handler when it gives a texture object's
// level a GS address and patches TEX0/MIPTBP1; the same code in the three
// games) is wrapped, and each placement is handed to the renderer in stream
// order as "blocks B.. hold level k of texture object O"
// (GSTextureObjectNote, PS2Memory::postGsSideband). The object's picture key
// is the size and index hash of its level 0 as the game holds it in RAM (the
// texels stay there for the level's lifetime), so it is the pack's key
// without reading GS memory. Draws find the object by TEX0's TBP0, or by
// MIPTBP1 when only mip levels 1 and up are uploaded.
#include "zge_xbox_textures.h"

#include "ps2_runtime.h"
#include "runtime/gs/gs_backend.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_texture_replace.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_session.h"

#include <chrono>
#include <cstring>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "sym/game_overrides/zge_xbox_textures.cpp.h"

namespace zge_host::xbox_textures
{
    // Named, not anonymous: the unity build puts other overrides in the
    // same translation unit.
    namespace pack
    {
        namespace tr = ps2x::texture_replace;
        namespace fs = std::filesystem;

        struct XboxTextures
        {
            std::mutex mutex;
            std::condition_variable cv;
            fs::path folder;            // <pack>/textures
            std::set<std::string> covered; // normalized PS2 paths with a .xtp
            std::deque<std::string> queue;
            std::set<std::string> queued;
            bool started = false;
            int trace = 0;
            // The loader thread (packWorker), stopped and joined when the
            // game is left; then the pictures it registered leave the
            // renderer (the next game booted brings its own).
            bool stop = false;
            std::thread worker;
            std::set<std::string> loaded; // the sources registered (packWorker's)

            ~XboxTextures()
            {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    stop = true;
                }
                cv.notify_all();
                if (worker.joinable())
                    worker.join();
                for (const std::string &source : loaded)
                    tr::removeSource(source);
            }
        };

        XboxTextures &packState() { return ps2x::session::state<XboxTextures>(); }

        bool readPackFile(const fs::path &file, std::vector<uint8_t> &out)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return false;
            in.seekg(0, std::ios::end);
            const std::streamoff size = in.tellg();
            if (size < 0)
                return false;
            in.seekg(0);
            out.resize(static_cast<size_t>(size));
            return size == 0 || static_cast<bool>(in.read(reinterpret_cast<char *>(out.data()), size));
        }

        void loadPack(XboxTextures &s, const std::string &ps2Path)
        {
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<uint8_t> bytes;
            const fs::path file = s.folder / (ps2Path + ".xtp");
            if (!readPackFile(file, bytes))
            {
                std::cerr << "[xbox] cannot read " << file.string() << std::endl;
                return;
            }
            const auto t1 = std::chrono::steady_clock::now();
            std::vector<tr::Entry> entries;
            std::string error;
            size_t skipped = 0;
            if (!tr::parsePack(bytes, entries, &error, &skipped))
            {
                std::cerr << "[xbox] " << file.string() << ": " << error << std::endl;
                return;
            }
            size_t pictureBytes = 0;
            std::set<const tr::Picture *> pictures;
            for (const tr::Entry &e : entries)
                if (pictures.insert(e.picture.get()).second)
                    pictureBytes += e.picture->data.size();
            const size_t count = pictures.size(), keys = entries.size();
            tr::setSource(ps2Path, std::move(entries));
            {
                std::lock_guard<std::mutex> lock(s.mutex);
                s.loaded.insert(ps2Path);
            }
            const auto t2 = std::chrono::steady_clock::now();
            const auto ms = [](auto a, auto b)
            { return std::chrono::duration<double, std::milli>(b - a).count(); };
            const tr::Stats st = tr::stats();
            std::cout << "[xbox] textures for " << ps2Path << ": " << count << " pictures (" << keys << " keys), "
                      << (pictureBytes >> 10)
                      << " KB, read " << ms(t0, t1) << " ms, parsed " << ms(t1, t2) << " ms"
                      << (skipped ? ", " + std::to_string(skipped) + " skipped" : std::string()) << " (held: "
                      << st.sources << " files, " << (st.bytes >> 20) << " MB)" << std::endl;
        }

        void packWorker(XboxTextures &s)
        {
            for (;;)
            {
                std::string path;
                {
                    std::unique_lock<std::mutex> lock(s.mutex);
                    s.cv.wait(lock, [&]()
                              { return s.stop || !s.queue.empty(); });
                    if (s.stop)
                        return;
                    path = s.queue.front();
                    s.queue.pop_front();
                }
                if (!tr::hasSource(path))
                    loadPack(s, path);
                std::lock_guard<std::mutex> lock(s.mutex);
                s.queued.erase(path);
            }
        }

        // Every .xtp under the folder, as normalized PS2 paths.
        void scanPack(XboxTextures &s)
        {
            std::error_code ec;
            for (fs::recursive_directory_iterator it(s.folder, ec), end; !ec && it != end; it.increment(ec))
            {
                if (!it->is_regular_file(ec))
                    continue;
                const fs::path rel = it->path().lexically_relative(s.folder);
                std::string name = rel.generic_string();
                if (name.size() <= 4u || name.compare(name.size() - 4u, 4u, ".xtp") != 0)
                    continue;
                s.covered.insert(ps2x::game_files::normalizePath(name.substr(0, name.size() - 4u)));
            }
        }
    }

    // ---- texture objects (identity for the renderer) ----------------------

    namespace objects_notes
    {
        namespace tr = ps2x::texture_replace;
        // The game's placeTextureLevel (a0 = texture object + 0x30, a1 = its level record).
        uint32_t placeTextureLevelOf(game::Game game)
        {
            switch (game)
            {
            case game::Game::Dm2:
                return sym::dm2::placeTextureLevel;
            case game::Game::Bmx:
                return sym::bmx::placeTextureLevel;
            case game::Game::Ai:
                break;
            }
            return sym::placeTextureLevel;
        }
        // Texture object (0x560 bytes, zge/render/texture.h Texture; the same in the three games):
        constexpr uint32_t kObjName = 0x10;      // char[32]
        constexpr uint32_t kObjLevels = 0x40;    // level records, 0xA0 each
        constexpr uint32_t kLevelStride = 0xA0;
        constexpr uint32_t kObjLevelCount = 0x4A0; // u8
        constexpr uint32_t kObjPsm = 0x4A1;        // u8
        // Level record: +0 texels in RAM, +8 GS word address, +0x0C u16
        // width, +0x0E u16 height, +0x14 words allocated.
        // The picture key of an object, kept while its level 0 looks the same.
        struct Key
        {
            uint32_t texels = 0, width = 0, height = 0, psm = 0;
            uint64_t sample = 0; // FNV-1a of 16 slices of the texels
            uint64_t hash = 0;
        };
        struct Objects
        {
            PS2Runtime::RecompiledFunction origAlloc = nullptr;
            int trace = 0; // PS2X_TRACE_XBOX_OBJECTS=1: each note, 2: and each key
            bool pack = false; // a pack is there (its sources may load later)
            std::function<bool(uint32_t, uint32_t, uint64_t)> keepOff; // keepTexturesOff
            uint64_t notes = 0;
            uint64_t keys = 0;

            std::unordered_map<uint32_t, Key> keyCache; // by texture object
        };
        Objects &objects() { return ps2x::session::state<Objects>(); }

        uint32_t u32At(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, 4);
            return v;
        }
        uint16_t u16At(const uint8_t *rdram, uint32_t address)
        {
            uint16_t v = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&v, p, 2);
            return v;
        }
        uint8_t u8At(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p ? *p : 0u;
        }

        // Host pointer to n guest bytes, or null if they leave RAM.
        const uint8_t *bytesAt(const uint8_t *rdram, uint32_t address, size_t n)
        {
            const uint8_t *first = getConstMemPtr(rdram, address);
            const uint8_t *last = n ? getConstMemPtr(rdram, address + static_cast<uint32_t>(n - 1u)) : first;
            return first && last == first + (n ? n - 1u : 0u) ? first : nullptr;
        }

        uint64_t sampleOf(const uint8_t *p, size_t bytes)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (uint32_t i = 0; i < 16u; ++i)
            {
                const size_t at = bytes > 8u ? (bytes - 8u) * i / 15u : 0u;
                for (size_t k = 0; k < 8u && at + k < bytes; ++k)
                    h = (h ^ p[at + k]) * 0x100000001B3ull;
            }
            return h;
        }

        // Size and index hash of the object's level 0 (0 hash: none).
        const Key *keyOf(const uint8_t *rdram, uint32_t object)
        {
            Key k;
            k.texels = u32At(rdram, object + kObjLevels);
            k.width = u16At(rdram, object + kObjLevels + 0x0C);
            k.height = u16At(rdram, object + kObjLevels + 0x0E);
            k.psm = u8At(rdram, object + kObjPsm);
            if ((k.psm != 0x13u && k.psm != 0x14u) || k.width == 0u || k.height == 0u || k.width > 1024u ||
                k.height > 1024u)
                return nullptr;
            const size_t bytes = static_cast<size_t>(k.width) * k.height / (k.psm == 0x14u ? 2u : 1u);
            const uint8_t *p = bytesAt(rdram, k.texels, bytes);
            if (!p)
                return nullptr;
            k.sample = sampleOf(p, bytes);
            // One index everywhere (a flat colour, or a texture the game
            // fills itself): its key says nothing about the picture.
            bool uniform = true;
            const uint8_t first = k.psm == 0x14u ? static_cast<uint8_t>((p[0] & 0x0Fu) * 0x11u) : p[0];
            for (size_t i = 0; i < bytes && uniform; ++i)
                uniform = p[i] == first;
            if (uniform)
                return nullptr;
            Key &cached = objects().keyCache[object];
            if (cached.hash != 0u && cached.texels == k.texels && cached.width == k.width && cached.height == k.height &&
                cached.psm == k.psm && cached.sample == k.sample)
                return &cached;
            k.hash = tr::indexHashOfUpload(p, k.width, k.height, k.psm);
            ++objects().keys;
            if (objects().trace > 1)
                std::cout << "[xbox] key of 0x" << std::hex << object << ": " << std::dec << k.width << "x" << k.height
                          << " psm 0x" << std::hex << k.psm << " hash " << k.hash << std::dec << std::endl;
            cached = k;
            return &cached;
        }

        void placeTextureLevelHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t object = getRegU32(ctx, 4) - 0x30u;
            const uint32_t record = getRegU32(ctx, 5);
            objects().origAlloc(rdram, ctx, runtime);
            // Only with pictures to show (sources may load after this).
            if (!tr::enabled() || !(objects().pack || tr::hasSources()))
                return;
            const uint32_t first = object + kObjLevels;
            const uint32_t count = u8At(rdram, object + kObjLevelCount);
            if (record < first || (record - first) % kLevelStride != 0u || (record - first) / kLevelStride >= count ||
                count > 7u)
                return;
            // Textures the game makes at run time ("Procedural:<n>", the
            // juice bar's fill and the message band) have no Xbox picture.
            if (const uint8_t *name = bytesAt(rdram, object + kObjName, 11u);
                name && std::memcmp(name, "Procedural:", 11u) == 0)
                return;
            const Key *key = keyOf(rdram, object);
            if (!key)
                return;
            GSTextureObjectNote n;
            n.object = object;
            n.level = static_cast<uint8_t>((record - first) / kLevelStride);
            n.tbp = u32At(rdram, record + 8u) >> 6;
            n.blocks = (u32At(rdram, record + 0x14u) + 63u) >> 6;
            n.width = u16At(rdram, record + 0x0Cu);
            n.height = u16At(rdram, record + 0x0Eu);
            n.psm = static_cast<uint8_t>(key->psm);
            n.keyWidth = static_cast<uint16_t>(key->width);
            n.keyHeight = static_cast<uint16_t>(key->height);
            n.keyHash = key->hash;
            if (objects().keepOff && objects().keepOff(key->width, key->height, key->hash))
                n.keyHash = 0u; // no picture has it (the tag at these blocks is replaced all the same)
            if (const uint8_t *name = bytesAt(rdram, object + kObjName, sizeof(n.name) - 1u))
                for (size_t i = 0; i + 1u < sizeof(n.name) && name[i]; ++i)
                    n.name[i] = static_cast<char>(name[i]);
            ++objects().notes;
            if (objects().trace)
                std::cout << "[xbox] object 0x" << std::hex << object << std::dec << " " << n.name << " level "
                          << unsigned(n.level) << " -> block " << n.tbp << " (" << n.blocks << ") " << n.width << "x"
                          << n.height << std::endl;
            runtime->memory().postGsSideband([runtime, notes = std::vector<GSTextureObjectNote>{n}]()
                                             { runtime->gs().noteTextureObjects(notes); });
        }

        // Once a session: a second call only says whether a pack is there.
        void install(PS2Runtime &runtime, game::Game game, bool pack)
        {
            objects().pack = objects().pack || pack;
            if (objects().origAlloc)
                return;
            if (const char *v = std::getenv("PS2X_TRACE_XBOX_OBJECTS"))
                objects().trace = std::atoi(v);
            if (const char *v = std::getenv("PS2X_XBOX_OBJECTS"); v && v[0] == '0')
                return; // debugging: textures not named (PS2X_TEXTURE_REPLACE_CONTENT=1 finds them by content)
            const uint32_t placeTextureLevel = placeTextureLevelOf(game);
            objects().origAlloc = runtime.lookupFunction(placeTextureLevel);
            if (!objects().origAlloc || !runtime.replaceFunction(placeTextureLevel, &placeTextureLevelHook))
            {
                objects().origAlloc = nullptr;
                std::cerr << "[xbox] texture cache hook not installed: no Xbox textures" << std::endl;
            }
        }
    }

    using namespace pack;

    ps2x::settings::Setting setting(std::function<void(bool)> save)
    {
        ps2x::settings::Setting row;
        row.key = kSettingKey;
        row.label = "Xbox Textures";
        row.help = "Sharper Xbox textures.";
        row.current = []()
        { return ps2x::texture_replace::enabled() ? 1 : 0; };
        row.apply = [save](int value)
        {
            ps2x::texture_replace::setEnabled(value != 0);
            if (save)
                save(value != 0);
        };
        return row;
    }

    std::string packFolder()
    {
        const char *pack = ps2x::boot::text("PS2X_XBOX_PACK");
        if (!pack || !pack[0])
            return {};
        std::error_code ec;
        return fs::is_directory(pack, ec) ? std::string(pack) : std::string();
    }

    void installTextureNotes(PS2Runtime &runtime, game::Game game)
    {
        objects_notes::install(runtime, game, false);
    }

    void install(PS2Runtime &runtime, game::Game game)
    {
        XboxTextures &s = packState();
        if (const char *v = std::getenv("PS2X_TRACE_XBOX"))
            s.trace = std::atoi(v);
        const char *on = ps2x::boot::text("PS2X_XBOX_TEXTURES");
        tr::setEnabled(!(on && on[0] == '0'));
        const std::string pack = packFolder();
        if (!pack.empty())
        {
            s.folder = fs::path(pack) / "textures";
            scanPack(s);
        }
        // The renderer learns which texture object each draw shows (also
        // for pictures other overrides register: AI's Zoo, DM2's Xbox levels).
        objects_notes::install(runtime, game, !s.covered.empty());
        if (pack.empty())
        {
            if (ps2x::boot::text("PS2X_XBOX_PACK"))
                std::cerr << "[xbox] PS2X_XBOX_PACK is not a folder" << std::endl;
            return;
        }
        if (s.covered.empty())
        {
            std::cout << "[xbox] no textures in " << pack << std::endl;
            return;
        }
        std::cout << "[xbox] textures for " << s.covered.size() << " files in " << s.folder.string() << " ("
                  << (tr::enabled() ? "on" : "off") << "; the GPU renderer draws them)" << std::endl;
        s.started = true;
        s.worker = std::thread(packWorker, std::ref(s));
        // The app adds the row with settings.ini behind it; the developer
        // runner gets one without (PS2X_CONTROL "set display.xbox_textures 0").
        if (!ps2x::settings::has(kSettingKey))
            ps2x::session::addSetting(setting(nullptr));
    }

    void keepTexturesOff(std::function<bool(uint32_t, uint32_t, uint64_t)> keepOff)
    {
        objects_notes::objects().keepOff = std::move(keepOff);
    }

    void noteFileOpened(const std::string &path)
    {
        XboxTextures &s = packState();
        if (!s.started)
            return;
        const std::string normalized = ps2x::game_files::normalizePath(path);
        if (!s.covered.count(normalized))
            return;
        if (s.trace)
            std::cout << "[xbox] opened " << normalized << std::endl;
        if (tr::hasSource(normalized))
            return;
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.queued.insert(normalized).second)
            return;
        s.queue.push_back(normalized);
        s.cv.notify_one();
    }
}
