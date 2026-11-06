# Box2D v3 (rigid bodies made from pixel clumps, phase 4). Static library,
# samples/tests/docs are hidden by Box2D itself when it is fetched.
include(FetchContent)

set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BOX2D_DOCS OFF CACHE BOOL "" FORCE)
set(BOX2D_VALIDATE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(box2d
    URL https://github.com/erincatto/box2d/archive/refs/tags/v3.2.0.tar.gz
    URL_HASH SHA256=FFEC747B27A2267016F8D0240D658C82D187FB2271FA0F69FCBE53FBC067180D
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(box2d)
