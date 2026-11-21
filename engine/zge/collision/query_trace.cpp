// The collision query's trace and timer (query_trace.h).

#include "query_trace.h"
#include "collision.h"

#include "ee/plain.h"
#include "zge/native/host.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include "sym/zge/collision/query_trace.cpp.h"

namespace zge
{
    namespace
    {
        struct QueryTrace
        {
            std::FILE *file = nullptr;
            bool time = false;
            uint64_t queries = 0;
            uint64_t nanos[2] = {0, 0}, counted[2] = {0, 0}; // sphere, segment
        };
        QueryTrace &queryTrace()
        {
            static QueryTrace t = []
            {
                QueryTrace q;
                if (const char *path = std::getenv("ZX_COLLISION_TRACE"); path && *path)
                    q.file = std::fopen(path, "w");
                const char *time = std::getenv("ZX_COLLISION_TIME");
                q.time = time && *time && std::atoi(time) != 0;
                return q;
            }();
            return t;
        }

        void traceQuery(QueryTrace &t, uint32_t kind, uint32_t flags, bool hit, uint64_t cycles)
        {
            std::fprintf(t.file, "%" PRIu64 " k%u f%x r%d c%" PRIu64, t.queries, kind, flags, hit ? 1 : 0, cycles);
            if (hit)
            {
                const QueryResults &r = *ee::at<QueryResults>(kQueryResultsAddress);
                std::fprintf(t.file, " n%d", r.count);
                for (int32_t i = 0; i < r.count && i < 128; ++i)
                {
                    const HitNode &h = *r.nodes[i];
                    std::fprintf(t.file, " [%x %x %u", h.node.address, h.part.address, h.faceCount);
                    for (uint32_t f = 0; f < h.faceCount && f < 64; ++f)
                    {
                        const FaceHit &face = h.faces[f];
                        std::fprintf(t.file, " %x:%08x,%08x,%08x,%08x,%08x", face.face, face.point.x.bits(),
                                     face.point.y.bits(), face.point.z.bits(), face.point.w.bits(),
                                     face.distance.bits());
                    }
                    std::fputc(']', t.file);
                }
            }
            std::fputc('\n', t.file);
        }
    }

    bool collisionQueryTraced()
    {
        const QueryTrace &t = queryTrace();
        return t.file || t.time;
    }

    bool traceCollisionQuery(uint32_t kind, uint32_t flags, const std::function<bool()> &query)
    {
        QueryTrace &t = queryTrace();
        const uint64_t cyclesBefore = native::eeCycle();
        const auto started = std::chrono::steady_clock::now();
        const bool hit = query();
        const uint64_t nanos = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
        ++t.queries;
        if (t.file)
            traceQuery(t, kind, flags, hit, native::eeCycle() - cyclesBefore);
        if (t.time && kind <= 1)
        {
            t.nanos[kind] += nanos;
            ++t.counted[kind];
            if (t.queries % 100000u == 0)
                std::printf("[collision-time] %" PRIu64 " queries: segments %" PRIu64 " at %.0f ns, spheres %" PRIu64
                            " at %.0f ns\n",
                            t.queries, t.counted[1], t.counted[1] ? double(t.nanos[1]) / double(t.counted[1]) : 0.0,
                            t.counted[0], t.counted[0] ? double(t.nanos[0]) / double(t.counted[0]) : 0.0);
        }
        return hit;
    }
}
