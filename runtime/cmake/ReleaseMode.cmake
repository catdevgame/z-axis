include(CheckIPOSupported)

option(ZX_ENABLE_LTO "Link-time optimization for Release builds" ON)

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

function(EnableFastReleaseMode TargetName)
    message("> Enabling optimization for: ${TargetName}")
    if(MSVC)
        # No /fp:fast: the game's float code and the VU emulation need IEEE
        # semantics (the same results as the other platforms' builds).
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release>:
                /O2 # speed
                /Ob2 # inline aggressively
                /Oi # intrinsics
                /Gy # function-level linking
                /Gw # global data in COMDAT
                /GF # string pooling
                /Zc:inline # remove unreferenced inline
                /DNDEBUG
                /GS- # Disable Buffer Security Check (faster)
            >
        )
        # cl's own whole program optimization; clang-cl gets LTO through
        # INTERPROCEDURAL_OPTIMIZATION below (lld-link).
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            target_compile_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:
                    /GL # whole program opt
                    /arch:AVX2 # Advanced Vector Extensions 2
                    /Qspectre- # Disable Spectre mitigations (faster)
                >
            )
            if(TARGET ${TargetName})
                target_link_options(${TargetName} PRIVATE
                    $<$<CONFIG:Release>:
                        /LTCG # link-time code generation
                    >
                )
            endif()
        endif()

        if(TARGET ${TargetName})
            # lld-link (clang-cl): fold only functions whose address is not
            # taken (/OPT:SAFEICF, LLD 19 on). Readable functions are
            # registered and called by their host address (game::call), and
            # a sweep's thousands of small drafts include many with the same
            # machine code; full ICF gave such pairs one address.
            if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
                set(_zx_icf /OPT:SAFEICF)
            else()
                set(_zx_icf /OPT:ICF)
            endif()
            target_link_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:
                    /OPT:REF # remove unreferenced
                    ${_zx_icf} # fold identical COMDATs (see above)
                >
            )
        endif()
    endif()

    if(NOT ZX_ENABLE_LTO)
        return()
    endif()
    if(IPO_SUPPORTED)
        set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    else()
        message(WARNING "Interprocedural optimization not supported: ${ipo_error}")
    endif()
endfunction()