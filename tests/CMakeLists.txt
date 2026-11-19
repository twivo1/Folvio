include(FetchContent)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(googletest)

add_executable(folvio_tests
    test_main.cpp
    TestSupport.h
    TestSupport.cpp
    test_core.cpp
    test_pdf.cpp
    test_autochange.cpp
    test_conversions.cpp
    test_editor.cpp
    test_regression_p0.cpp
    test_combine.cpp
    test_combine_interaction.cpp)
target_link_libraries(folvio_tests PRIVATE folvio_lib GTest::gtest Qt6::Test)
target_compile_definitions(folvio_tests PRIVATE FOLVIO_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures")
add_custom_command(TARGET folvio_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PDFIUM_RUNTIME}" "$<TARGET_FILE_DIR:folvio_tests>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/engines" "$<TARGET_FILE_DIR:folvio_tests>/engines")
add_test(NAME folvio_tests COMMAND folvio_tests)
set_tests_properties(folvio_tests PROPERTIES TIMEOUT 1800)
