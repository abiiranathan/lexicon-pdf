cmake_minimum_required(VERSION 3.20)

# Emit build/compile_commands.json so editors (clangd, VS Code C/C++) can
# resolve third-party headers (poppler/cairo/glib live outside the default
# include path and are only known via pkg-config).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(lexicon-pdf
    VERSION      1.1.0
    DESCRIPTION  "Poppler-backed PDF processing library"
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

option(LEXICON_PDF_BUILD_TESTS
    "Build the lexicon-pdf regression suite (run with `make test` / ctest)"
    ON
)

# ── Dependencies ──────────────────────────────────────────────────────────────

find_package(PkgConfig REQUIRED)
pkg_check_modules(Poppler REQUIRED IMPORTED_TARGET poppler-glib)

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

# poppler-glib headers are only needed when compiling our .c file.
target_link_libraries(lexicon-pdf
    PRIVATE
        PkgConfig::Poppler
)

# Expose our own public header directory to consumers.
target_include_directories(lexicon-pdf
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(lexicon-pdf
    # _GNU_SOURCE unlocks POSIX extensions for consumers too.
    PUBLIC  _GNU_SOURCE
)

target_compile_options(lexicon-pdf PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
)

# ── Regression suite (`make test` / ctest) ────────────────────────────────────

if(LEXICON_PDF_BUILD_TESTS)
    include(CTest)

    add_executable(lexicon-pdf-tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pdf.c
    )

    set_target_properties(lexicon-pdf-tests PROPERTIES
        C_STANDARD          11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS        OFF
    )

    # The test includes "lexicon_pdf.h" from the source root and links the
    # in-tree library (poppler link flags propagate from it automatically).
    target_include_directories(lexicon-pdf-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(lexicon-pdf-tests PRIVATE
        lexicon::pdf
    )

    target_compile_options(lexicon-pdf-tests PRIVATE
        -Wall -Wextra -Wpedantic
    )

    add_test(NAME pdf-api COMMAND lexicon-pdf-tests)
endif()

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
