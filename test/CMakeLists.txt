include_directories(${PROJECT_SOURCE_DIR}/src)

add_executable(ComPtr_test ComPtr_test.cpp)
target_link_libraries(ComPtr_test gtest_main gmock_main)
add_test(NAME ComPtr_test COMMAND ComPtr_test)

add_executable(ComObject_test ComObject_test.cpp)
target_link_libraries(ComObject_test gtest_main gmock_main)
add_test(NAME ComObject_test COMMAND ComObject_test)

add_executable(CandidateWindow_test CandidateWindow_test.cpp)
target_compile_definitions(CandidateWindow_test PRIVATE UNICODE _UNICODE)
# Chinese string literals: don't let MSVC read the UTF-8 source as the ANSI code page (950 on zh-TW)
target_compile_options(CandidateWindow_test PRIVATE /utf-8)
target_link_libraries(CandidateWindow_test libIME2_static gtest_main gmock_main)
add_test(NAME CandidateWindow_test COMMAND CandidateWindow_test)

add_executable(KeyEvent_test KeyEvent_test.cpp)
target_link_libraries(KeyEvent_test libIME2_static gtest_main gmock_main)
add_test(NAME KeyEvent_test COMMAND KeyEvent_test)

add_executable(CandidateAccessibility_test CandidateAccessibility_test.cpp)
target_compile_definitions(CandidateAccessibility_test PRIVATE UNICODE _UNICODE)
target_compile_options(CandidateAccessibility_test PRIVATE /utf-8)
target_link_libraries(CandidateAccessibility_test libIME2_static gtest_main gmock_main ole32 oleaut32)
add_test(NAME CandidateAccessibility_test COMMAND CandidateAccessibility_test)

# Fuzzing the candidate window (fuzz/CandidateWindow_fuzz.cpp). The saved inputs
# (fuzz/seeds, fuzz/regressions) run on every build as an ordinary test.
add_executable(CandidateWindow_fuzz_replay fuzz/CandidateWindow_fuzz.cpp fuzz/replay_main.cpp)
target_compile_features(CandidateWindow_fuzz_replay PRIVATE cxx_std_17)
target_compile_options(CandidateWindow_fuzz_replay PRIVATE /utf-8)
target_link_libraries(CandidateWindow_fuzz_replay libIME2_static ole32 oleaut32)
add_test(NAME CandidateWindow_fuzz_replay
         COMMAND CandidateWindow_fuzz_replay ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/seeds ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/regressions)

# -DLIBIME2_FUZZ=ON (MSVC, x64, Release): the libFuzzer target, with libIME2
# compiled again under AddressSanitizer and coverage instrumentation.
#   CandidateWindow_fuzz.exe corpus_dir fuzz/seeds -max_total_time=600
# clang_rt.asan_dynamic-x86_64.dll (MSVC's bin\Hostx64\x64) must be on PATH.
option(LIBIME2_FUZZ "Build the libFuzzer targets (MSVC /fsanitize=fuzzer,address, x64)" OFF)
if(LIBIME2_FUZZ)
    get_target_property(LIBIME2_SOURCES libIME2_static SOURCES)
    get_target_property(LIBIME2_SOURCE_DIR libIME2_static SOURCE_DIR)
    list(TRANSFORM LIBIME2_SOURCES PREPEND "${LIBIME2_SOURCE_DIR}/")
    set(LIBIME2_FUZZ_FLAGS /fsanitize=address /fsanitize=fuzzer /Zi)
    add_library(libIME2_fuzz STATIC ${LIBIME2_SOURCES})
    target_compile_options(libIME2_fuzz PRIVATE ${LIBIME2_FUZZ_FLAGS})
    add_executable(CandidateWindow_fuzz fuzz/CandidateWindow_fuzz.cpp)
    target_compile_features(CandidateWindow_fuzz PRIVATE cxx_std_17)
    target_compile_options(CandidateWindow_fuzz PRIVATE /utf-8 ${LIBIME2_FUZZ_FLAGS})
    target_link_options(CandidateWindow_fuzz PRIVATE /INCREMENTAL:NO /DEBUG)
    target_link_libraries(CandidateWindow_fuzz libIME2_fuzz shlwapi oleaut32 ole32)
endif()
