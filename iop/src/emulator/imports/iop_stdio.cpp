#include "iop_stdio.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "ps2x/iop/iop_host.h"

#include <cstdio>
#include <string>

namespace ps2x::iop::detail
{
    namespace
    {
        // Minimal printf formatter for IOP modules (o32 varargs: a1..a3, then
        // the caller's stack from sp+0x10). Handles flags/width/precision with
        // d i u x X o c s p and %%; enough to make driver logs readable.
        std::string formatIop(IopMemory &memory, const IopCpuState &cpu, uint32_t format, int firstArg)
        {
            const std::string fmt = memory.readString(format, 2048u);
            int argIndex = firstArg;
            const auto nextArg = [&]() -> uint32_t
            {
                const int index = argIndex++;
                if (index < 4)
                    return cpu.gpr[4 + index];
                return memory.read32(cpu.gpr[29] + 0x10u + static_cast<uint32_t>(index - 4) * 4u);
            };

            std::string out;
            for (size_t i = 0; i < fmt.size(); ++i)
            {
                if (fmt[i] != '%')
                {
                    out.push_back(fmt[i]);
                    continue;
                }
                size_t j = i + 1;
                std::string spec = "%";
                while (j < fmt.size() && std::string_view("-+ #0123456789.lh").find(fmt[j]) != std::string_view::npos)
                {
                    if (fmt[j] != 'l' && fmt[j] != 'h')
                        spec.push_back(fmt[j]);
                    ++j;
                }
                if (j >= fmt.size())
                    break;
                const char conv = fmt[j];
                char buffer[512];
                switch (conv)
                {
                case 'd':
                case 'i':
                    spec.push_back('d');
                    std::snprintf(buffer, sizeof(buffer), spec.c_str(), static_cast<int32_t>(nextArg()));
                    out += buffer;
                    break;
                case 'u':
                case 'x':
                case 'X':
                case 'o':
                case 'c':
                    spec.push_back(conv);
                    std::snprintf(buffer, sizeof(buffer), spec.c_str(), static_cast<unsigned>(nextArg()));
                    out += buffer;
                    break;
                case 'p':
                    std::snprintf(buffer, sizeof(buffer), "0x%08x", static_cast<unsigned>(nextArg()));
                    out += buffer;
                    break;
                case 's':
                {
                    const std::string text = memory.readString(nextArg(), 1024u);
                    spec.push_back('s');
                    std::snprintf(buffer, sizeof(buffer), spec.c_str(), text.c_str());
                    out += buffer;
                    break;
                }
                case '%':
                    out.push_back('%');
                    break;
                default:
                    out.append(fmt, i, j - i + 1);
                    break;
                }
                i = j;
            }
            return out;
        }
    }

    IopStdio::IopStdio(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    bool IopStdio::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        const auto logString = [&](std::string_view prefix, uint32_t address, uint32_t resultBias = 0u)
        {
            const std::string text = m_memory.readString(address, 2048u);
            m_host.log(LogLevel::Info, std::string(prefix) + text);
            setV0(static_cast<uint32_t>(text.size()) + resultBias);
        };

        switch (ordinal)
        {
        case 4: // printf
        {
            const std::string text = formatIop(m_memory, cpu, a0, 1);
            m_host.log(LogLevel::Info, "[IOP printf] " + text);
            setV0(static_cast<uint32_t>(text.size()));
            return true;
        }
        case 5: // getchar
        case 10:
            setV0(0xFFFFFFFFu);
            return true;
        case 6: // putchar
            m_host.log(LogLevel::Info, std::string("[IOP putchar] ") + static_cast<char>(a0 & 0xFFu));
            setV0(a0 & 0xFFu);
            return true;
        case 7: // puts
            logString("[IOP puts] ", a0, 1u);
            return true;
        case 8: // gets
        case 13:
            setV0(0u);
            return true;
        case 9: // fdprintf
            logString("[IOP fdprintf] ", a1);
            return true;
        case 11:
            setV0(a0 & 0xFFu);
            return true;
        case 12: // fdputs
            logString("[IOP fdputs] ", a0);
            return true;
        case 14: // vfdprintf
            logString("[IOP vfdprintf] ", a1);
            return true;
        default:
            return false;
        }
    }
}
