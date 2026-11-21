// The body of the tests' two game modules (zxa/, zxb/; tests/CMakeLists.txt): each a module of its own, built as
// a game's generated code is (cmake/game_module.cmake) and linked into zx-tests as the runner links each game's
// (ZX_GAME_MODULES=static). What a recompiler's register_functions.cpp is, at its smallest: a two-slot table, its
// filler and its registration. ps2_game_module_tests.cpp checks that each registers its own code.
// TEST_MODULE: the module's namespace (as dm2's and bmx's code has one); TEST_MODULE_ID, TEST_MODULE_ELF: its
// names; TEST_MODULE_BASE: the guest address of its first slot.
namespace TEST_MODULE
{
extern const uint32_t g_ps2RecompiledFunctionTableBase = TEST_MODULE_BASE;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = TEST_MODULE_BASE + 8u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 2u;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[2u] = {};

namespace
{
void firstFunction(uint8_t *, R5900Context *ctx, PS2Runtime *) { ctx->pc = TEST_MODULE_BASE; }
struct GeneratedFunctionTableInitializer
{
    GeneratedFunctionTableInitializer() { g_ps2RecompiledFunctionTable[0] = firstFunction; }
};
static const GeneratedFunctionTableInitializer g_generatedFunctionTableInitializer;
}
} // namespace TEST_MODULE

static const ps2_generated::Registrar g_ps2GeneratedCodeRegistrar{TEST_MODULE_ID, TEST_MODULE_ELF,
    TEST_MODULE::g_ps2RecompiledFunctionTableBase, TEST_MODULE::g_ps2RecompiledFunctionTableEnd,
    TEST_MODULE::g_ps2RecompiledFunctionTableSlotCount, TEST_MODULE::g_ps2RecompiledFunctionTable};
