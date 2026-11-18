cmake_minimum_required(VERSION 3.24)

project(Folvio
    VERSION 0.10.0
    DESCRIPTION "Folvio - universal file workspace"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FOLVIO_BUILD_TESTS "Build the test suite" ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Sql Svg Concurrent)
if(FOLVIO_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
endif()
if(UNIX AND NOT APPLE)
    include(GNUInstallDirs)
    # Private library directory (keeps our libpdfium.so away from any system copy).
    set(FOLVIO_PRIVATE_LIBDIR "${CMAKE_INSTALL_LIBDIR}/folvio")
    set(CMAKE_BUILD_RPATH "$ORIGIN")
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../${FOLVIO_PRIVATE_LIBDIR}")
endif()

# ---------------------------------------------------------------------------
# PDFium (prebuilt, BSD-3 / Apache-2.0) - third_party/pdfium
# ---------------------------------------------------------------------------
if(WIN32)
    set(PDFIUM_ROOT "${CMAKE_SOURCE_DIR}/third_party/pdfium" CACHE PATH "PDFium binary package root")
    set(PDFIUM_RUNTIME "${PDFIUM_ROOT}/bin/pdfium.dll")
    set(PDFIUM_FETCH "tools/fetch_pdfium.ps1")
else()
    set(PDFIUM_ROOT "${CMAKE_SOURCE_DIR}/third_party/pdfium-linux" CACHE PATH "PDFium binary package root")
    set(PDFIUM_RUNTIME "${PDFIUM_ROOT}/lib/libpdfium.so")
    set(PDFIUM_FETCH "tools/fetch_pdfium.sh")
endif()
if(NOT EXISTS "${PDFIUM_ROOT}/include/fpdfview.h" OR NOT EXISTS "${PDFIUM_RUNTIME}")
    message(FATAL_ERROR "PDFium not found in ${PDFIUM_ROOT}. Run ${PDFIUM_FETCH} first.")
endif()
add_library(pdfium SHARED IMPORTED GLOBAL)
set_target_properties(pdfium PROPERTIES
    IMPORTED_LOCATION "${PDFIUM_RUNTIME}"
    INTERFACE_INCLUDE_DIRECTORIES "${PDFIUM_ROOT}/include")
if(MINGW)
    # GNU ld links directly against the DLL; no MSVC import library needed.
    set_target_properties(pdfium PROPERTIES IMPORTED_IMPLIB "${PDFIUM_ROOT}/bin/pdfium.dll")
elseif(WIN32)
    set_target_properties(pdfium PROPERTIES IMPORTED_IMPLIB "${PDFIUM_ROOT}/lib/pdfium.dll.lib")
endif()

# ---------------------------------------------------------------------------
# OpenCV (core + imgproc, Apache-2.0), built statically by tools/build_opencv.ps1 / .sh
# ---------------------------------------------------------------------------
set(OpenCV_STATIC ON)
find_package(OpenCV 4.10 REQUIRED COMPONENTS core imgproc
             PATHS "${CMAKE_SOURCE_DIR}/third_party/opencv" NO_DEFAULT_PATH)

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
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Sql Qt6::Svg Qt6::Concurrent pdfium ${OpenCV_LIBS})
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
if(WIN32)
    set_target_properties(folvio PROPERTIES OUTPUT_NAME "Folvio")
else()
    set_target_properties(folvio PROPERTIES OUTPUT_NAME "folvio")
endif()

# Copy runtime pieces next to the executable for development runs.
add_custom_command(TARGET folvio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PDFIUM_RUNTIME}" "$<TARGET_FILE_DIR:folvio>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/engines" "$<TARGET_FILE_DIR:folvio>/engines")

if(WIN32)
    install(TARGETS folvio RUNTIME DESTINATION .)
    install(FILES "${PDFIUM_RUNTIME}" DESTINATION .)
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/engines/" DESTINATION engines)
else()
    # FHS layout, used by the .deb package and as the AppImage AppDir (prefix /usr).
    set(FOLVIO_APP_ID "io.github.twivo1.Folvio")
    install(TARGETS folvio RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES "${PDFIUM_RUNTIME}" DESTINATION "${FOLVIO_PRIVATE_LIBDIR}")
    install(DIRECTORY "${CMAKE_SOURCE_DIR}/engines/tessdata" DESTINATION "${CMAKE_INSTALL_DATADIR}/folvio/engines")
    install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/${FOLVIO_APP_ID}.desktop" DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES "${CMAKE_SOURCE_DIR}/packaging/linux/${FOLVIO_APP_ID}.metainfo.xml" DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
    foreach(size 16 32 48 64 128 256 512)
        install(FILES "${CMAKE_SOURCE_DIR}/assets/branding/png/folvio-icon-${size}.png"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps" RENAME "${FOLVIO_APP_ID}.png")
    endforeach()
    install(FILES "${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_SOURCE_DIR}/THIRD_PARTY_LICENSES.md"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/folvio")
    install(FILES "${PDFIUM_ROOT}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/folvio/licenses/pdfium")
    install(DIRECTORY "${PDFIUM_ROOT}/licenses/" DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/folvio/licenses/pdfium")
    install(FILES "${CMAKE_SOURCE_DIR}/third_party/opencv/share/licenses/opencv4/LICENSE"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/folvio/licenses/opencv" OPTIONAL)
endif()

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
