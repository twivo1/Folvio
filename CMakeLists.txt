cmake_minimum_required(VERSION 3.24)

project(Folvio
    VERSION 0.9.0
    DESCRIPTION "Folvio - universal file workspace"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FOLVIO_BUILD_TESTS "Build the test suite" ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Sql Svg Concurrent Test)

# ---------------------------------------------------------------------------
# PDFium (prebuilt, BSD-3 / Apache-2.0) - third_party/pdfium
# ---------------------------------------------------------------------------
set(PDFIUM_ROOT "${CMAKE_SOURCE_DIR}/third_party/pdfium" CACHE PATH "PDFium binary package root")
if(NOT EXISTS "${PDFIUM_ROOT}/include/fpdfview.h")
    message(FATAL_ERROR "PDFium not found in ${PDFIUM_ROOT}. Run tools/fetch_pdfium.ps1 first.")
endif()
add_library(pdfium SHARED IMPORTED GLOBAL)
set_target_properties(pdfium PROPERTIES
    IMPORTED_LOCATION "${PDFIUM_ROOT}/bin/pdfium.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${PDFIUM_ROOT}/include")
if(MINGW)
    # GNU ld links directly against the DLL; no MSVC import library needed.
    set_target_properties(pdfium PROPERTIES IMPORTED_IMPLIB "${PDFIUM_ROOT}/bin/pdfium.dll")
else()
    set_target_properties(pdfium PROPERTIES IMPORTED_IMPLIB "${PDFIUM_ROOT}/lib/pdfium.dll.lib")
endif()

# ---------------------------------------------------------------------------
# Core library: everything except main(). Linked by the app and the tests.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE FOLVIO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h")
list(FILTER FOLVIO_SOURCES EXCLUDE REGEX ".*/src/app/main\\.cpp$")

add_library(folvio_lib STATIC ${FOLVIO_SOURCES} "${CMAKE_SOURCE_DIR}/assets/resources.qrc")
target_include_directories(folvio_lib PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(folvio_lib PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Sql Qt6::Svg Qt6::Concurrent pdfium)
target_compile_definitions(folvio_lib PUBLIC
    FOLVIO_VERSION="${PROJECT_VERSION}"
    UNICODE _UNICODE)
if(WIN32)
    target_link_libraries(folvio_lib PUBLIC shell32 ole32 uuid)
endif()
if(MINGW OR CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(folvio_lib PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
set(APP_RC "")
if(WIN32)
    configure_file("${CMAKE_SOURCE_DIR}/packaging/windows/folvio.rc.in"
                   "${CMAKE_BINARY_DIR}/folvio.rc" @ONLY)
    set(APP_RC "${CMAKE_BINARY_DIR}/folvio.rc")
endif()

add_executable(folvio WIN32 src/app/main.cpp ${APP_RC})
target_link_libraries(folvio PRIVATE folvio_lib)
set_target_properties(folvio PROPERTIES OUTPUT_NAME "Folvio")

# Copy runtime pieces next to the executable for development runs.
add_custom_command(TARGET folvio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PDFIUM_ROOT}/bin/pdfium.dll" "$<TARGET_FILE_DIR:folvio>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/engines" "$<TARGET_FILE_DIR:folvio>/engines")

install(TARGETS folvio RUNTIME DESTINATION .)
install(FILES "${PDFIUM_ROOT}/bin/pdfium.dll" DESTINATION .)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/engines/" DESTINATION engines)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(FOLVIO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Developer diagnostics (not installed)
# ---------------------------------------------------------------------------
option(FOLVIO_BUILD_TOOLS "Build developer diagnostic tools" ON)
if(FOLVIO_BUILD_TOOLS)
    add_executable(pdfdiag tools/pdfdiag/pdfdiag.cpp)
    target_link_libraries(pdfdiag PRIVATE folvio_lib)
endif()
