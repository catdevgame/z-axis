// Pictures at given vsyncs (runtime/gs_shots.h).

#include "runtime/gs_shots.h"

#include "host_backend.h"
#include "machine.h"
#include "gs/gs_frontend.h"
#include "runtime/boot_config.h"
#include "runtime/memory.h"
#include "runtime/session.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace zx::gs_shots
{
    namespace
    {
        struct Shots
        {
            bool installed = false;
            std::vector<uint64_t> ticks;
            std::string prefix;
            // ZX_GS_SHOT_PRIMS=N: the primitives of the N vsyncs before each picture, to PREFIX-vT.prims
            // (GS::setPrimitiveLog), for comparing two render paths draw by draw.
            uint64_t primVsyncs = 0;
            std::FILE *prims = nullptr;
            // ZX_GS_SHOT_TRACE=1 (with ZX_GS_SHOT_PRIMS=N): a GS trace of the same vsyncs to PREFIX-vT.gsr
            // (GS::startRecording), for replaying the frame on another renderer (zx-gsreplay --gpu).
            bool trace = false;
        };
    }

    void install(PS2Machine &runtime)
    {
        Shots &s = zx::session::state<Shots>();
        const char *spec = zx::boot::text("ZX_GS_SHOT_VSYNC");
        if (s.installed || !spec || !spec[0])
            return;
        const std::string text = spec;
        const size_t colon = text.find(':');
        if (colon == std::string::npos)
            return;
        s.prefix = text.substr(colon + 1);
        size_t pos = 0;
        const std::string list = text.substr(0, colon);
        while (pos < list.size())
        {
            size_t comma = list.find(',', pos);
            if (comma == std::string::npos)
                comma = list.size();
            s.ticks.push_back(std::strtoull(list.substr(pos, comma - pos).c_str(), nullptr, 0));
            pos = comma + 1;
        }
        std::sort(s.ticks.begin(), s.ticks.end());
        if (const char *n = zx::boot::text("ZX_GS_SHOT_PRIMS"); n && n[0])
            s.primVsyncs = std::strtoull(n, nullptr, 0);
        if (const char *t = zx::boot::text("ZX_GS_SHOT_TRACE"); t && t[0] == '1')
            s.trace = true;
        s.installed = true;
        PS2Machine *rt = &runtime;
        auto shots = std::make_shared<Shots>(s);
        runtime.addVSyncHook([rt, shots](uint8_t *, uint64_t tick)
        {
            if (shots->primVsyncs && std::binary_search(shots->ticks.begin(), shots->ticks.end(), tick + shots->primVsyncs))
            {
                const std::string path = shots->prefix + "-v" + std::to_string(tick + shots->primVsyncs) + ".prims";
                rt->memory().postGsSideband([rt, shots, path]()
                {
                    if (shots->prims)
                        std::fclose(shots->prims);
                    shots->prims = std::fopen(path.c_str(), "wb");
                    rt->gs().setPrimitiveLog(shots->prims);
                    if (shots->trace)
                        rt->gs().startRecording(path.substr(0, path.size() - 6) + ".gsr",
                                                static_cast<uint32_t>(shots->primVsyncs + 1));
                });
            }
            if (!std::binary_search(shots->ticks.begin(), shots->ticks.end(), tick))
                return;
            const std::string path = shots->prefix + "-v" + std::to_string(tick) + ".png";
            // The display as the GS stream has it at this vsync, read on the GS worker: a swap the EE queued as GS
            // work (an HLE sceGsSwapDBuff) may not have run yet when the EE gets here, and the EE may already be
            // drawing the next frame into the buffer its registers still show.
            rt->memory().postGsSideband([rt, shots, path]()
            {
                if (shots->prims)
                {
                    rt->gs().setPrimitiveLog(nullptr);
                    std::fclose(shots->prims);
                    shots->prims = nullptr;
                }
                const PresentationFrame frame = rt->gs().pictureOfDisplay(rt->gs().displayNow());
                const bool ok = frame && ps2_host::writePng(path, frame.pixels.data(), frame.width, frame.height,
                                                            frame.width * 4u);
                std::cerr << "[gs-shot] " << path << (ok ? "" : " (no picture)") << std::endl;
            });
        });
        std::cerr << "[gs-shot] " << s.ticks.size() << " vsyncs to " << s.prefix << std::endl;
    }
}
