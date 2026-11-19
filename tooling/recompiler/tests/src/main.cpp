#include "MiniTest.h"

#include <cstdlib>
#include <iostream>

void reset_ps2_test_function_table();
void register_code_generator_tests();
void register_r5900_decoder_tests();
void register_elf_analyzer_tests();
void register_ps2_recompiler_tests();
void register_ps2_runtime_expansion_tests();
void register_ps2_float_semantics_tests();
void register_ps2_branch64_tests();

int main()
{
    MiniTest::BeforeEach(reset_ps2_test_function_table);
    register_code_generator_tests();
    register_r5900_decoder_tests();
    register_elf_analyzer_tests();
    register_ps2_recompiler_tests();
    register_ps2_runtime_expansion_tests();
    register_ps2_float_semantics_tests();
    register_ps2_branch64_tests();
    int res = MiniTest::Run();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(res);
}
