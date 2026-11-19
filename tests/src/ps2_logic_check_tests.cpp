// Tests for the game-logic comparison tooling: the null GS raster backend
// (PS2X_GS_NULL) and the VBlank state probe (PS2X_DUMP_WHEN, PS2X_STATE_LOG).
#include "MiniTest.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_null_backend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_pad_script.h"
#include "runtime/ps2_state_probe.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
    constexpr uint8_t kGifFmtImage = 2u;

    void appendU64(std::vector<uint8_t> &dst, uint64_t value)
    {
        const size_t pos = dst.size();
        dst.resize(pos + sizeof(uint64_t));
        std::memcpy(dst.data() + pos, &value, sizeof(uint64_t));
    }

    struct DrawAndUploadResult
    {
        uint32_t drawnPixel = 0u;
        uint32_t uploadedPixel = 0u;
        uint32_t readbackPixel = 0u;
    };

    // Draws one point at (0,0) of a CT32 frame at block 0, uploads one CT32
    // pixel to block 64 through an IMAGE transfer, then reads it back with a
    // local-to-host transfer.
    DrawAndUploadResult drawAndUpload(bool nullBackend)
    {
        std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
        GS gs;
        gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);
        if (nullBackend)
            gs.setRasterBackend(std::make_unique<GSNullBackend>(std::make_unique<GSCpuBackend>()));
        else
            gs.setRasterBackend(std::make_unique<GSCpuBackend>());

        gs.writeRegister(GS_REG_FRAME_1, 1ull << 16);
        gs.writeRegister(GS_REG_ZBUF_1, 32ull | (1ull << 32)); // Z at block 32, writes masked
        gs.writeRegister(GS_REG_SCISSOR_1, 0ull);
        gs.writeRegister(GS_REG_TEST_1, (1ull << 16) | (1ull << 17)); // ZTE, ZTST=ALWAYS
        gs.writeRegister(GS_REG_PRIM, static_cast<uint64_t>(GS_PRIM_POINT));
        gs.writeRegister(GS_REG_RGBAQ, 0x80112233ull | (0x3F800000ull << 32));
        gs.writeRegister(GS_REG_XYZ2, 0ull);

        constexpr uint32_t kUploadBlock = 64u;
        gs.writeRegister(GS_REG_BITBLTBUF, (static_cast<uint64_t>(kUploadBlock) << 32) | (1ull << 48));
        gs.writeRegister(GS_REG_TRXPOS, 0ull);
        gs.writeRegister(GS_REG_TRXREG, 1ull | (1ull << 32));
        gs.writeRegister(GS_REG_TRXDIR, 0ull);
        std::vector<uint8_t> packet;
        appendU64(packet, 1ull | (1ull << 15) | (static_cast<uint64_t>(kGifFmtImage) << 58));
        appendU64(packet, 0ull);
        appendU64(packet, 0x00000000CAFEF00Dull);
        appendU64(packet, 0ull);
        gs.processGIFPacket(packet.data(), static_cast<uint32_t>(packet.size()));

        DrawAndUploadResult result;
        result.drawnPixel = gs.ReadVram(GS_PSM_CT32, 0u, 1u, 0u, 0u);
        result.uploadedPixel = gs.ReadVram(GS_PSM_CT32, kUploadBlock, 1u, 0u, 0u);

        gs.writeRegister(GS_REG_BITBLTBUF, static_cast<uint64_t>(kUploadBlock) | (1ull << 16));
        gs.writeRegister(GS_REG_TRXPOS, 0ull);
        gs.writeRegister(GS_REG_TRXREG, 1ull | (1ull << 32));
        gs.writeRegister(GS_REG_TRXDIR, 1ull);
        uint8_t readback[16]{};
        if (gs.consumeLocalToHostBytes(readback, sizeof(readback)) >= 4u)
            std::memcpy(&result.readbackPixel, readback, sizeof(result.readbackPixel));
        return result;
    }

    void writeU32(std::vector<uint8_t> &ram, uint32_t address, uint32_t value)
    {
        std::memcpy(ram.data() + address, &value, sizeof(value));
    }
}

void register_ps2_logic_check_tests()
{
    MiniTest::Case("LogicCheck", [](TestCase &tc)
    {
        tc.Run("null GS backend drops primitives but keeps transfers", [](TestCase &t)
        {
            const DrawAndUploadResult cpu = drawAndUpload(false);
            const DrawAndUploadResult null = drawAndUpload(true);
            t.Equals(cpu.drawnPixel, 0x80112233u, "CPU backend should draw the point");
            t.Equals(null.drawnPixel, 0u, "null backend should not draw the point");
            t.Equals(cpu.uploadedPixel, 0xCAFEF00Du, "CPU backend should store the uploaded pixel");
            t.Equals(null.uploadedPixel, 0xCAFEF00Du, "null backend should store the uploaded pixel");
            t.Equals(null.readbackPixel, cpu.readbackPixel, "local-to-host readback should match the CPU backend");
            t.Equals(null.readbackPixel, 0xCAFEF00Du, "local-to-host readback should return the uploaded pixel");
        });

        tc.Run("state probe dump triggers fire once, in order", [](TestCase &t)
        {
            StateProbe probe;
            std::string error;
            t.IsTrue(probe.parseDumpSpec("menu:0x1000==1,level:0x1004<100&0x1000==2,late:tick>=50", &error),
                     "spec should parse: " + error);
            t.Equals(probe.triggers().size(), static_cast<size_t>(3), "three triggers");

            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            std::vector<std::string> fired;
            auto writer = [&fired](const std::string &label, uint64_t tick)
            { fired.push_back(label + "@" + std::to_string(tick)); };

            writeU32(ram, 0x1004, 5u); // small counter before the first trigger: must not fire "level"
            writeU32(ram, 0x1000, 2u);
            probe.onVsync(ram.data(), 1, writer);
            t.IsTrue(fired.empty(), "level must wait until menu fired");
            writeU32(ram, 0x1000, 1u);
            writeU32(ram, 0x1004, 500u);
            probe.onVsync(ram.data(), 2, writer);
            probe.onVsync(ram.data(), 3, writer);
            writeU32(ram, 0x1000, 2u);
            probe.onVsync(ram.data(), 4, writer);
            writeU32(ram, 0x1004, 0u);
            probe.onVsync(ram.data(), 5, writer);
            probe.onVsync(ram.data(), 6, writer);
            probe.onVsync(ram.data(), 50, writer);
            probe.onVsync(ram.data(), 51, writer);
            t.Equals(fired.size(), static_cast<size_t>(3), "each trigger fires exactly once");
            if (fired.size() == 3u)
            {
                t.Equals(fired[0], std::string("menu@2"), "menu at tick 2");
                t.Equals(fired[1], std::string("level@5"), "level at tick 5");
                t.Equals(fired[2], std::string("late@50"), "late at tick 50");
            }
        });

        tc.Run("pad script accepts seconds and vsync units", [](TestCase &t)
        {
            PadScript script;
            std::vector<std::string> warnings;
            script.parse("1.00:START:0.50,120v:CROSS:6v,10v:LX=-1:2v,5v:BOGUS", &warnings);
            t.Equals(script.presses().size(), static_cast<size_t>(3), "three presses");
            t.Equals(warnings.size(), static_cast<size_t>(1), "one unknown entry");
            uint8_t sticks[4] = {0x80, 0x80, 0x80, 0x80};
            t.Equals(script.update(1000.0, 0, nullptr, sticks), static_cast<uint16_t>(0), "origin: nothing");
            t.Equals(script.update(1010.0, 0, nullptr, sticks), static_cast<uint16_t>(0), "stick only at 10v");
            t.Equals(sticks[PadScript::kAxisLx], static_cast<uint8_t>(1), "LX held left (128 - 127.5, rounded)");
            t.Equals(script.update(1060.0, 0, nullptr, sticks), static_cast<uint16_t>(0x0008u), "START at 1 s");
            t.Equals(script.update(1120.0, 0, nullptr, sticks), static_cast<uint16_t>(0x4000u), "CROSS at 120v");
            t.Equals(script.update(1126.0, 0, nullptr, sticks), static_cast<uint16_t>(0), "CROSS released at 126v");
        });

        tc.Run("pad script SYNC jumps forward on an early event and waits for a late one", [](TestCase &t)
        {
            PadScript script;
            script.parse("100v:SYNC=0x2000==1,102v:CROSS:2v,200v:SYNC=0x2000==2&0x2004>=7,201v:CIRCLE:1v");
            t.Equals(script.syncs().size(), static_cast<size_t>(2), "two anchors");
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            PadScript::SyncEvent event{};

            script.update(0.0, 0, ram.data(), nullptr);
            script.update(40.0, 0, ram.data(), nullptr);
            writeU32(ram, 0x2000, 1u); // event at clock 50, recorded at 100
            event.index = 99;
            script.update(50.0, 0, ram.data(), nullptr, &event);
            t.Equals(event.index, static_cast<size_t>(0), "first anchor hit");
            t.Equals(event.shift, 50.0, "clock jumped forward 50 vsyncs");
            t.Equals(script.scriptTime(), 100.0, "script time is the anchor time");
            t.Equals(script.update(52.0, 0, ram.data(), nullptr), static_cast<uint16_t>(0x4000u),
                     "CROSS follows the anchor by 2 vsyncs");

            // Clock reaches 200 (raw 150) before the second event: hold there.
            script.update(150.0, 0, ram.data(), nullptr);
            script.update(170.0, 0, ram.data(), nullptr);
            t.Equals(script.scriptTime(), 200.0, "held at the anchor");
            writeU32(ram, 0x2000, 2u);
            script.update(171.0, 0, ram.data(), nullptr);
            t.Equals(script.scriptTime(), 200.0, "still held: second condition false");
            writeU32(ram, 0x2004, 7u);
            event.index = 99;
            script.update(180.0, 0, ram.data(), nullptr, &event);
            t.Equals(event.index, static_cast<size_t>(1), "second anchor hit");
            t.Equals(event.shift, -30.0, "clock stood 30 vsyncs");
            t.Equals(script.update(181.0, 0, ram.data(), nullptr), static_cast<uint16_t>(0x2000u),
                     "CIRCLE 1 vsync after the late event");
            t.Equals(script.update(182.0, 0, ram.data(), nullptr), static_cast<uint16_t>(0),
                     "CIRCLE released");
        });

        tc.Run("pad script SYNC chains on the same state take one event each", [](TestCase &t)
        {
            PadScript script;
            script.parse("10v:SYNC=0x3000==1,20v:SYNC=0x3000==1,30v:SYNC=0x3000==1");
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            writeU32(ram, 0x3000, 1u); // already true at the start: not an event
            script.update(0.0, 0, ram.data(), nullptr);
            t.Equals(script.nextSync(), static_cast<size_t>(0), "true from the start is not an event");
            writeU32(ram, 0x3000, 0u);
            script.update(1.0, 0, ram.data(), nullptr);
            writeU32(ram, 0x3000, 1u);
            script.update(2.0, 0, ram.data(), nullptr);
            t.Equals(script.nextSync(), static_cast<size_t>(1), "first box");
            script.update(3.0, 0, ram.data(), nullptr);
            script.update(4.0, 0, ram.data(), nullptr);
            t.Equals(script.nextSync(), static_cast<size_t>(1), "same box again is not the next event");
            writeU32(ram, 0x3000, 0u);
            script.update(5.0, 0, ram.data(), nullptr);
            writeU32(ram, 0x3000, 1u);
            script.update(6.0, 0, ram.data(), nullptr);
            t.Equals(script.nextSync(), static_cast<size_t>(2), "second box");
        });

        tc.Run("pad script WAIT holds when late and never jumps ahead", [](TestCase &t)
        {
            PadScript script;
            script.parse("50v:WAIT=0x4000==1,51v:CROSS:1v,100v:WAIT=0x4000==2,101v:CIRCLE:1v");
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            PadScript::SyncEvent event{};
            writeU32(ram, 0x4000, 1u); // condition true early: no jump
            script.update(0.0, 0, ram.data(), nullptr);
            script.update(10.0, 0, ram.data(), nullptr);
            t.Equals(script.scriptTime(), 10.0, "early condition does not move the clock");
            event.index = 99;
            script.update(50.0, 0, ram.data(), nullptr, &event);
            t.Equals(event.index, static_cast<size_t>(0), "first WAIT passes at its time");
            t.Equals(event.shift, 0.0, "no shift when on time");
            t.Equals(script.update(51.0, 0, ram.data(), nullptr), static_cast<uint16_t>(0x4000u), "CROSS on time");
            // Late: stand at 100 until the condition holds.
            script.update(100.0, 0, ram.data(), nullptr);
            script.update(110.0, 0, ram.data(), nullptr);
            t.Equals(script.scriptTime(), 100.0, "held at the WAIT");
            writeU32(ram, 0x4000, 2u);
            event.index = 99;
            script.update(115.0, 0, ram.data(), nullptr, &event);
            t.Equals(event.index, static_cast<size_t>(1), "second WAIT passes");
            t.Equals(event.shift, -15.0, "stood 15 vsyncs");
            t.Equals(script.update(116.0, 0, ram.data(), nullptr), static_cast<uint16_t>(0x2000u), "CIRCLE one vsync later");
        });

        tc.Run("state probe log ranges and pad-read logging", [](TestCase &t)
        {
            StateProbe probe;
            t.IsTrue(probe.parseLogSpec("0x100:3,0x200"), "range spec should parse");
            const std::vector<uint32_t> expected{0x100u, 0x104u, 0x108u, 0x200u};
            t.IsTrue(probe.logAddresses() == expected, "0x100:3 lists three consecutive words");
            t.IsTrue(!probe.parseLogSpec("0x100:0"), "zero words is malformed");
            t.IsTrue(!probe.parseLogSpec("0x100:x"), "bad word count");

            // A pad-read probe fires its dump triggers like the VBlank one.
            StateProbe pad;
            t.IsTrue(pad.parseDumpSpec("f:0x1000==7"), "dump spec");
            std::vector<uint8_t> ram(PS2_RAM_SIZE, 0u);
            std::vector<std::string> fired;
            auto writer = [&fired](const std::string &label, uint64_t tick)
            { fired.push_back(label + "@" + std::to_string(tick)); };
            pad.onPadRead(ram.data(), 1, writer);
            writeU32(ram, 0x1000, 7u);
            pad.onPadRead(ram.data(), 2, writer);
            pad.onPadRead(ram.data(), 3, writer);
            t.Equals(fired.size(), static_cast<size_t>(1), "fires once");
            t.IsTrue(!fired.empty() && fired[0] == "f@2", "fires at the first read where the condition holds");
        });

        tc.Run("state probe rejects malformed specs", [](TestCase &t)
        {
            StateProbe probe;
            t.IsTrue(!probe.parseDumpSpec("nolabel"), "missing label");
            t.IsTrue(!probe.parseDumpSpec("a:0x10=1"), "single = is not an operator");
            t.IsTrue(!probe.parseDumpSpec("a:0x10==zz"), "bad value");
            t.IsTrue(!probe.parseLogSpec("0x10,foo"), "bad log address");
            t.IsTrue(probe.parseLogSpec("0x10,0x20"), "good log spec");
            StateProbe::Condition condition{};
            t.IsTrue(StateProbe::parseCondition("script>=2365", condition), "script clock condition");
            StateProbe::publishScriptTime(-1);
            t.IsTrue(!StateProbe::evaluate(condition, nullptr, 0), "script clock not started");
            StateProbe::publishScriptTime(2365);
            t.IsTrue(StateProbe::evaluate(condition, nullptr, 0), "script clock reached");
            StateProbe::publishScriptTime(-1);
        });
    });
}
