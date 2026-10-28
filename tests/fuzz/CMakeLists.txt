# Fuzz targets (tests/fuzz), libFuzzer-style (LLVMFuzzerTestOneInput); see README.md.
# With -DFA_FUZZ=ON (a separate build tree, sanitizers on) this builds fuzz_<target>, each
# with fuzz_driver.c, the coverage-guided driver for compilers without libFuzzer.
# Otherwise it builds fuzz_regress_<target> and runs every input in regress/<target>
# (inputs that once crashed or hung a reader) as an ordinary test.
set(FA_FUZZ_TARGETS stream tags zip hls)
foreach(t ${FA_FUZZ_TARGETS})
    if(FA_FUZZ)
        add_executable(fuzz_${t} fuzz_${t}.c fuzz_driver.c)
        target_link_libraries(fuzz_${t} PRIVATE fa_whitebox)
    else()
        add_executable(fuzz_regress_${t} fuzz_${t}.c regress_main.c)
        target_compile_options(fuzz_regress_${t} PRIVATE ${FA_WARNINGS})
        target_link_libraries(fuzz_regress_${t} PRIVATE fa_whitebox)
        if(MSVC)
            set_property(TARGET fuzz_regress_${t} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        add_test(NAME fuzz_regress_${t} COMMAND fuzz_regress_${t} ${CMAKE_CURRENT_SOURCE_DIR}/regress/${t})
    endif()
endforeach()
