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
