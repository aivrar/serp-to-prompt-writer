cmake_minimum_required(VERSION 3.16)
project(serp_to_prompt_writer C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── Find vcpkg packages ─────────────────────────────────────
find_package(CURL REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(unofficial-gumbo CONFIG QUIET)
find_package(ZLIB REQUIRED)

# Gumbo fallback (same pattern as E:\searcher)
if(NOT unofficial-gumbo_FOUND)
    find_path(GUMBO_INCLUDE_DIR gumbo.h)
    find_library(GUMBO_LIBRARY NAMES gumbo gumbo-parser)
    if(GUMBO_INCLUDE_DIR AND GUMBO_LIBRARY)
        set(GUMBO_FOUND TRUE)
    endif()
endif()

# ONNX Runtime — pre-built from GitHub releases
set(ORT_DIR ${CMAKE_SOURCE_DIR}/lib/onnxruntime/onnxruntime-win-x64-1.17.1)
if(EXISTS ${ORT_DIR}/include/onnxruntime_c_api.h)
    set(ORT_FOUND TRUE)
    set(ORT_INCLUDE_DIR ${ORT_DIR}/include)
    set(ORT_LIBRARY ${ORT_DIR}/lib/onnxruntime.lib)
    message(STATUS "ONNX Runtime found: ${ORT_DIR}")
else()
    set(ORT_FOUND FALSE)
    message(STATUS "ONNX Runtime not found — building without USE_ONNX")
endif()

# ── Sources ──────────────────────────────────────────────────
set(SOURCES
    src/main.c
    src/app_log.c
    src/config.c
    src/utils.c
    src/sysinfo.c
    src/threadpool.c
    src/resmon.c
    src/proxy.c
    src/filter.c
    src/serper.c
    src/scraper.c
    src/js_render.c
    src/database.c
    src/nlp.c
    src/onnx_nlp.c
    src/nli.c
    src/prompt.c
    src/engine.c
    src/tui.c
    lib/cJSON/cJSON.c
)

# ── Win32 resources (embedded data files) ───────────────────
if(WIN32)
    list(APPEND SOURCES src/resources.rc)
endif()

add_executable(serp_to_prompt_writer WIN32 ${SOURCES})

# ── Include paths ────────────────────────────────────────────
target_include_directories(serp_to_prompt_writer PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/cJSON
)

if(GUMBO_INCLUDE_DIR)
    target_include_directories(serp_to_prompt_writer PRIVATE ${GUMBO_INCLUDE_DIR})
endif()

if(ORT_INCLUDE_DIR)
    target_include_directories(serp_to_prompt_writer PRIVATE ${ORT_INCLUDE_DIR})
endif()

# ── Link libraries ───────────────────────────────────────────
target_link_libraries(serp_to_prompt_writer PRIVATE
    CURL::libcurl
    unofficial::sqlite3::sqlite3
    ZLIB::ZLIB
)

if(unofficial-gumbo_FOUND)
    target_link_libraries(serp_to_prompt_writer PRIVATE unofficial::gumbo::gumbo)
elseif(GUMBO_FOUND)
    target_link_libraries(serp_to_prompt_writer PRIVATE ${GUMBO_LIBRARY})
endif()

if(ORT_FOUND)
    target_include_directories(serp_to_prompt_writer PRIVATE ${ORT_INCLUDE_DIR})
    target_link_libraries(serp_to_prompt_writer PRIVATE ${ORT_LIBRARY})
    target_compile_definitions(serp_to_prompt_writer PRIVATE USE_ONNX=1)
endif()

# ── Windows ──────────────────────────────────────────────────
if(WIN32)
    target_link_libraries(serp_to_prompt_writer PRIVATE
        ws2_32 winmm winhttp comctl32 user32 gdi32 shell32 advapi32 psapi ole32
    )
    target_compile_definitions(serp_to_prompt_writer PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
    )
    target_link_options(serp_to_prompt_writer PRIVATE /STACK:8388608)
endif()

# ── Smoke test (console app) ─────────────────────────────────
set(TEST_SOURCES
    src/test_smoke.c
    src/app_log.c
    src/config.c
    src/utils.c
    src/sysinfo.c
    src/threadpool.c
    src/resmon.c
    src/proxy.c
    src/filter.c
    src/serper.c
    src/scraper.c
    src/js_render.c
    src/database.c
    src/nlp.c
    src/onnx_nlp.c
    src/nli.c
    src/prompt.c
    src/engine.c
    lib/cJSON/cJSON.c
)

add_executable(test_smoke ${TEST_SOURCES})
set_target_properties(test_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test")

target_include_directories(test_smoke PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/cJSON
)
if(GUMBO_INCLUDE_DIR)
    target_include_directories(test_smoke PRIVATE ${GUMBO_INCLUDE_DIR})
endif()
if(ORT_INCLUDE_DIR)
    target_include_directories(test_smoke PRIVATE ${ORT_INCLUDE_DIR})
endif()

target_link_libraries(test_smoke PRIVATE
    CURL::libcurl
    unofficial::sqlite3::sqlite3
    ZLIB::ZLIB
)
if(unofficial-gumbo_FOUND)
    target_link_libraries(test_smoke PRIVATE unofficial::gumbo::gumbo)
elseif(GUMBO_FOUND)
    target_link_libraries(test_smoke PRIVATE ${GUMBO_LIBRARY})
endif()
if(ORT_FOUND)
    target_link_libraries(test_smoke PRIVATE ${ORT_LIBRARY})
    target_compile_definitions(test_smoke PRIVATE USE_ONNX=1)
endif()

if(WIN32)
    target_link_libraries(test_smoke PRIVATE
        ws2_32 winmm winhttp comctl32 user32 gdi32 shell32 advapi32 psapi ole32
    )
    target_compile_definitions(test_smoke PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
    )
    target_link_options(test_smoke PRIVATE /STACK:8388608 /SUBSYSTEM:CONSOLE)
endif()

