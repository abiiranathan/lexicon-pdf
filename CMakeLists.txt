cmake_minimum_required(VERSION 3.20)

project(lexicon-pdf
    VERSION      1.0.0
    DESCRIPTION  "PDFium-backed PDF processing library"
    HOMEPAGE_URL "https://github.com/abiiranathan/lexicon"
    LANGUAGES    C
)

# ── Options ──────────────────────────────────────────────────────────────────

option(LEXICON_PDF_INSTALL
    "Generate install targets and pkg-config file"
    ON
)

option(BUILD_SHARED_LIBS
    "Build lexicon-pdf as a shared library"
    OFF
)

# ── Dependencies ──────────────────────────────────────────────────────────────

find_library(PDFIUM_LIB pdfium
    REQUIRED
    HINTS /usr/local/lib /usr/lib
)

find_path(PDFIUM_INCLUDE fpdfview.h
    REQUIRED
    HINTS /usr/local/include /usr/include
)

# ── Library target ────────────────────────────────────────────────────────────

add_library(lexicon-pdf
    ${CMAKE_CURRENT_SOURCE_DIR}/lexicon_pdf.c
)

# Canonical alias so consumers can write target_link_libraries(... lexicon::pdf)
# whether they use add_subdirectory() or find_package().
add_library(lexicon::pdf ALIAS lexicon-pdf)

set_target_properties(lexicon-pdf PROPERTIES
    C_STANDARD          11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS        OFF
    # Embed the version in the shared library soname, e.g. liblexicon-pdf.so.1
    VERSION             ${PROJECT_VERSION}
    SOVERSION           ${PROJECT_VERSION_MAJOR}
    # Public header exposed to consumers
    PUBLIC_HEADER       ${CMAKE_CURRENT_SOURCE_DIR}/lexicon_pdf.h
    # Export name used in the CMake package config
    EXPORT_NAME         pdf
)

# pdfium headers are only needed when compiling our .c file.
target_include_directories(lexicon-pdf
    PRIVATE
        ${PDFIUM_INCLUDE}
    # Expose our own public header directory to consumers.
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(lexicon-pdf
    PRIVATE
        ${PDFIUM_LIB}
)

target_compile_definitions(lexicon-pdf
    # _GNU_SOURCE unlocks POSIX extensions (iconv, etc.) for consumers too.
    PUBLIC  _GNU_SOURCE
)

target_compile_options(lexicon-pdf PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
)

# ── Install ───────────────────────────────────────────────────────────────────

if(LEXICON_PDF_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    # -- Library + header -----------------------------------------------------

    install(TARGETS lexicon-pdf
        EXPORT          lexicon-pdf-targets
        ARCHIVE         DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY         DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME         DESTINATION ${CMAKE_INSTALL_BINDIR}
        PUBLIC_HEADER   DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lexicon
    )

    # -- CMake package config -------------------------------------------------

    # Generate lexicon-pdf-config-version.cmake
    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/lexicon-pdf-config-version.cmake"
        VERSION         ${PROJECT_VERSION}
        COMPATIBILITY   SameMajorVersion
    )

    # Generate the targets file (lexicon-pdf-targets.cmake)
    install(EXPORT lexicon-pdf-targets
        FILE        lexicon-pdf-targets.cmake
        NAMESPACE   lexicon::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lexicon-pdf
    )

    # Generate lexicon-pdf-config.cmake from the .in template below.
    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/lexicon-pdf-config.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/lexicon-pdf-config.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lexicon-pdf
    )

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/lexicon-pdf-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/lexicon-pdf-config-version.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/lexicon-pdf
    )

    # -- pkg-config -----------------------------------------------------------

    # Resolve the pdfium library name for the .pc Libs.private field.
    get_filename_component(PDFIUM_LIB_NAME "${PDFIUM_LIB}" NAME_WE)
    string(REGEX REPLACE "^lib" "" PDFIUM_LIB_NAME "${PDFIUM_LIB_NAME}")

    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/lexicon-pdf.pc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/lexicon-pdf.pc"
        @ONLY
    )

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/lexicon-pdf.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )
endif()
