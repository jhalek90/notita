# Luau (sandboxed scripting for data files such as data/reactions.luau).
# Only the compiler and VM are built; the CLI, analysis and tests are skipped.
include(FetchContent)

set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)

FetchContent_Declare(luau
    URL https://github.com/luau-lang/luau/archive/refs/tags/0.745.tar.gz
    URL_HASH SHA256=53C22773BACE6CABBABAEAD576FBC08D11A0317FCE11A3DBF9D027DDA6849248
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(luau)
if(NOT luau_POPULATED)
    FetchContent_Populate(luau)
    # EXCLUDE_FROM_ALL: only the Luau libraries we link get built
    add_subdirectory(${luau_SOURCE_DIR} ${luau_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
