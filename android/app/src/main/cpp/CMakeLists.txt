cmake_minimum_required(VERSION 3.28)

project(cadpp_android LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GameActivity ships as a Prefab-backed AAR. AGP extracts the headers
# and static library into the build tree and exposes this CONFIG.
find_package(game-activity REQUIRED CONFIG)

if(NOT DEFINED CADPP_LIB_DIR)
    message(FATAL_ERROR
        "CADPP_LIB_DIR not set. Build cadpp first:\n"
        "  cd <repo>; mise exec -- exon build --target aarch64-linux-android\n"
        "then rerun ./gradlew assembleDebug.")
endif()

if(NOT DEFINED LIBREDWG_LIB)
    set(LIBREDWG_LIB "${CADPP_LIB_DIR}/_deps/libredwg-build/libredwg.a")
endif()

if(NOT EXISTS "${LIBREDWG_LIB}")
    message(FATAL_ERROR
        "LIBREDWG_LIB not found at ${LIBREDWG_LIB}. exon's "
        "aarch64-linux-android target must have produced it under "
        "_deps/libredwg-build/. Re-run `exon build --target "
        "aarch64-linux-android` from the cadpp root.")
endif()

function(cadpp_import_static name path)
    add_library(${name} STATIC IMPORTED)
    set_target_properties(${name} PROPERTIES IMPORTED_LOCATION "${path}")
endfunction()

# Top-level archives exon emits for cadpp's aarch64-linux-android
# target. cadpp path-deps phenotype, so phenotype's modules + cppx + txn
# + jsoncpp + the generated std module all land alongside libcadpp.a.
cadpp_import_static(cadpp_static
    "${CADPP_LIB_DIR}/libcadpp.a")
cadpp_import_static(cadpp_phenotype_static
    "${CADPP_LIB_DIR}/libphenotype.a")
cadpp_import_static(cadpp_cppx_static
    "${CADPP_LIB_DIR}/libcppx.a")
cadpp_import_static(cadpp_txn_static
    "${CADPP_LIB_DIR}/libtxn.a")
cadpp_import_static(cadpp_jsoncpp_static
    "${CADPP_LIB_DIR}/libjsoncpp.a")
cadpp_import_static(cadpp_cxx_std_static
    "${CADPP_LIB_DIR}/lib__cmake_cxx_std_23.a")
cadpp_import_static(cadpp_libredwg_static
    "${LIBREDWG_LIB}")

add_library(cadpp_android SHARED
    cadpp_android_main.cpp
)

# AGP defaults to `-Wl,--fatal-warnings`, which turns ld.lld's section-
# group COMDAT-mismatch *warning* into a hard error when phenotype's
# `inline` module-scope state (g_renderer, dialog state, …) is emitted
# into both `libphenotype.a` *and* `libcadpp.a` (cadpp imports
# `phenotype.native.android` from its Android entry point).
# Deduplication still picks one prevailing copy at runtime — the
# warning is benign here, so let it stay a warning.
target_link_options(cadpp_android PRIVATE -Wl,--no-fatal-warnings)

target_link_libraries(cadpp_android PRIVATE
    # --start-group lets the static archives resolve each other's
    # symbols regardless of command-line order. cadpp depends on
    # phenotype + libredwg; phenotype depends on cppx + txn + jsoncpp +
    # the generated std module — without the group, the linker would
    # need a hand-tuned order that breaks every time exon adds a dep.
    -Wl,--start-group
    # Phenotype's archive must come before cadpp's so its inline
    # module-scope state (`g_renderer`, dialog state, …) wins COMDAT
    # tie-breaking. Otherwise ld.lld picks cadpp's transitively-emitted
    # copy as prevailing while phenotype's `.init_array` still
    # references its own discarded section, blowing up the link with
    # "relocation refers to a discarded section".
    cadpp_phenotype_static
    cadpp_static
    cadpp_cppx_static
    cadpp_txn_static
    cadpp_jsoncpp_static
    cadpp_cxx_std_static
    cadpp_libredwg_static
    -Wl,--end-group
    # GameActivity's JNI entry points (Java_com_google_androidgamesdk_*)
    # are only referenced from Java, so without --whole-archive the
    # linker strips them and the JVM throws UnsatisfiedLinkError at
    # onCreate time.
    -Wl,--whole-archive
    game-activity::game-activity_static
    -Wl,--no-whole-archive
    vulkan
    android
    # phenotype's text pipeline (AndroidBitmap_*, AImageDecoder_*) lives
    # in libjnigraphics; required even though cad++ M6c does not draw
    # bitmap text yet — the symbols are referenced from libphenotype.a
    # whether or not cad++ exercises that path.
    jnigraphics
    log)
