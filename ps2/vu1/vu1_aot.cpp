// zx-vu1aot: ahead-of-time compiled C++ for VU1 micro memory images.
//
//   zx-vu1aot <out-dir> <input>...
//
// Inputs are VU1 corpora (ZX_VU1_RECORD, every micro memory image their
// runs used) or raw 16 KiB images (ZX_VU1_CODE_DUMP, *.vu1code). For each
// distinct image it writes <out-dir>/vu1_aot_<hash>.cpp and its chunks (the
// generator: vu1/vu1_aot_generator.h, ps2/vu1/vu1_aot_program.h).
// Build them into the runner with -DZX_VU1_AOT_DIR=<out-dir>; the app's
// first run builds them into a game's module.
#include "vu1/vu1_interpreter.h"
#include "vu1/vu1_aot_generator.h"
#include "vu1/vu1_record.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: zx-vu1aot <out-dir> <corpus.vu1r | image.vu1code>...\n");
        return 2;
    }
    std::map<uint64_t, std::vector<uint8_t>> images;
    for (int i = 2; i < argc; ++i)
    {
        const std::string path = argv[i];
        if (std::filesystem::path(path).extension() == ".vu1r")
        {
            ps2vu1rec::Reader reader;
            if (!reader.open(path))
            {
                std::fprintf(stderr, "cannot read %s\n", path.c_str());
                return 1;
            }
            ps2vu1rec::Vu1RecordedRun run;
            std::vector<uint8_t> code, dataIn, dataOut;
            uint32_t lastCodeId = ~0u;
            while (reader.next(run, code, dataIn, dataOut))
            {
                if (run.codeId == lastCodeId)
                    continue;
                lastCodeId = run.codeId;
                images[VU1Interpreter::codeHash(code.data(), static_cast<uint32_t>(code.size()))] = code;
            }
            continue;
        }
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> code((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (code.size() != 0x4000u)
        {
            std::fprintf(stderr, "%s: not a 16 KiB micro memory image\n", path.c_str());
            return 1;
        }
        images[VU1Interpreter::codeHash(code.data(), static_cast<uint32_t>(code.size()))] = code;
    }
    for (const auto &[hash, code] : images)
    {
        zx::vu1aot::Written written;
        std::string error;
        if (!zx::vu1aot::writeProgram(code, argv[1], written, error))
        {
            std::fprintf(stderr, "%016llx: %s\n", static_cast<unsigned long long>(hash), error.c_str());
            return 1;
        }
        for (const std::filesystem::path &file : written.files)
            std::printf("%s\n", file.string().c_str());
        std::printf("%u blocks, %u of %u pairs\n", written.blocks, written.pairsInBlocks, 0x4000u / 8u);
    }
    return 0;
}
