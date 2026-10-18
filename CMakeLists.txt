cmake_minimum_required(VERSION 3.24)
project(vex LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 17)

# Everything ships into one folder so the runtime (which resolves vexdrv.sys and
# kernel_payload.dll relative to its own path) finds them next to the exe.
# Defaults to dist\ inside the repo; override with -DVEX_OUTPUT_DIR=...
# (build.bat passes it through).
if(NOT DEFINED VEX_OUTPUT_DIR OR VEX_OUTPUT_DIR STREQUAL "")
    set(VEX_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/dist" CACHE PATH "where bs_esp.exe is assembled")
endif()
file(MAKE_DIRECTORY "${VEX_OUTPUT_DIR}")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${VEX_OUTPUT_DIR})
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} ${VEX_OUTPUT_DIR})
endforeach()

# ---------------------------------------------------------------------------
# ImGui  (vendored under third_party/imgui so the tree stays self-contained)
# ---------------------------------------------------------------------------
#  Prefer the vendored checkout, then an explicit IMGUI_DIR, and only fall back
#  to FetchContent (which needs git) when neither exists.
# ---------------------------------------------------------------------------
if(NOT DEFINED IMGUI_DIR AND EXISTS "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.h")
    set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui")
    message(STATUS "vex: using vendored ImGui from third_party/imgui")
endif()

if(DEFINED IMGUI_DIR AND EXISTS "${IMGUI_DIR}/imgui.h")
    message(STATUS "vex: using ImGui from ${IMGUI_DIR}")
    add_library(imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
    )
    target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    target_compile_definitions(imgui PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(imgui PUBLIC dwmapi)
else()
    find_package(Git QUIET)
    if(NOT Git_FOUND)
        message(FATAL_ERROR
            "git was not found and IMGUI_DIR was not set.\n"
            "Either install git, or unpack ImGui and configure with:\n"
            "    cmake -S . -B build -G \"Visual Studio 17 2022\" -A x64 -DIMGUI_DIR=C:/path/to/imgui")
    endif()

    include(FetchContent)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui
        GIT_TAG v1.90.9
    )
    FetchContent_MakeAvailable(imgui)
endif()

# ---------------------------------------------------------------------------
#  bs_probe.exe  --  runtime discovery for the Blood Strike target
#
#  Deliberately standalone: OpenProcess + ReadProcessMemory only, so it answers
#  "is the usermode read path viable?" before any kernel component is involved.
# ---------------------------------------------------------------------------
add_executable(bs_probe src/bs_probe.cpp)
target_compile_definitions(bs_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(bs_probe PRIVATE advapi32)

# ---------------------------------------------------------------------------
#  chain_hunt.exe  --  locate the engine chain by structure
#
#  Separate from bs_probe because it answers one question fast: every published
#  offset for ClientEngine reads back as ASCII in this build, so the address has
#  to be rediscovered from the chain's shape.
# ---------------------------------------------------------------------------
add_executable(chain_hunt src/chain_hunt.cpp)
target_compile_definitions(chain_hunt PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(chain_hunt PRIVATE advapi32)

# ---------------------------------------------------------------------------
#  chain_test.exe  --  validate a supplied offset set against the live process
# ---------------------------------------------------------------------------
add_executable(chain_test src/chain_test.cpp)
target_compile_definitions(chain_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(chain_test PRIVATE advapi32)

# ---------------------------------------------------------------------------
#  struct_scan.exe  --  resolve ActorProps and the entity list by measurement
# ---------------------------------------------------------------------------
add_executable(struct_scan src/struct_scan.cpp)
target_compile_definitions(struct_scan PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(struct_scan PRIVATE advapi32)

# ---------------------------------------------------------------------------
#  entity_list.exe  --  locate the container holding every player
# ---------------------------------------------------------------------------
add_executable(entity_list src/entity_list.cpp)
target_compile_definitions(entity_list PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(entity_list PRIVATE advapi32)

# ---------------------------------------------------------------------------
#  wnd_test.exe  --  minimal reproduction of the overlay window crash
# ---------------------------------------------------------------------------
add_executable(wnd_test src/wnd_test.cpp src/overlay.cpp src/stealth.cpp)
target_include_directories(wnd_test PRIVATE src)
target_compile_definitions(wnd_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(wnd_test PRIVATE user32 d3d11 dxgi dwmapi shcore)
if(TARGET imgui)
    target_link_libraries(wnd_test PRIVATE imgui)
endif()

# ---------------------------------------------------------------------------
#  bs_esp.exe  --  Blood Strike PC: player discovery by motion, and the ESP
# ---------------------------------------------------------------------------
add_executable(bs_esp WIN32
  src/bs_esp.cpp
  src/aim.cpp
  src/menu.cpp
  src/overlay.cpp
  src/stealth.cpp
)
target_include_directories(bs_esp PRIVATE src shared)
target_compile_definitions(bs_esp PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_link_libraries(bs_esp PRIVATE
    d3d11 dxgi dwmapi shcore user32 gdi32 shell32 advapi32 ntdll winmm)
target_link_options(bs_esp PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup)

# ImGui is declared further down as the `imgui` target; link it here as well, and
# guard for the FetchContent path where that target carries a different name.
if(TARGET imgui)
    target_link_libraries(bs_esp PRIVATE imgui)
elseif(TARGET imgui_static)
    target_link_libraries(bs_esp PRIVATE imgui_static)
endif()

# ---------------------------------------------------------------------------
#  premul_test.exe  --  exhaustive check of the SIMD premultiply in overlay.cpp
#  aim_test.exe     --  selection and smoothing arithmetic, against simulated data
#
#  Both are console programs with no dependency on the game or on D3D: they test
#  arithmetic that has a computable right answer.  premul_test in particular
#  caught a real bug in the SSE2 alpha broadcast that no screenshot would have
#  shown -- see its header comment.
# ---------------------------------------------------------------------------
add_executable(premul_test src/premul_test.cpp)
target_include_directories(premul_test PRIVATE src)
target_compile_definitions(premul_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

add_executable(aim_test src/aim_test.cpp)
target_compile_definitions(aim_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# skeleton_filter_test -- the player/scenery decision, against the two real
# objects from the run that produced no boxes.  No game, no D3D.
add_executable(skeleton_filter_test src/skeleton_filter_test.cpp)
target_compile_definitions(skeleton_filter_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# premul_bench -- not a test, a measurement.  Establishes whether the CPU
# premultiply is the slow phase without needing a game to be running, so that
# question is settled before looking at the live breakdown.
add_executable(premul_bench src/premul_bench.cpp)
target_compile_definitions(premul_bench PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(premul_bench PRIVATE user32 gdi32)

# ---------------------------------------------------------------------------
#  bone_find.exe  --  locate the bone array by shared translation
#
#  For the run that rejected 1389 pool candidates down to 2 with the comment
#  "non-human bone shape", which means +0x208/+0x214 are no longer the bones.
#  Needs a live match and a MOVING player; see the file header.
# ---------------------------------------------------------------------------
add_executable(bone_find src/bone_find.cpp)
target_compile_definitions(bone_find PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(bone_find PRIVATE advapi32 user32)

# ---------------------------------------------------------------------------
#  players.exe  --  find every player by the measured object signature
# ---------------------------------------------------------------------------
add_executable(players src/players.cpp)
target_compile_definitions(players PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(players PRIVATE advapi32)

# ---------------------------------------------------------------------------
#  pos_find.exe  --  locate the position field by temporal differential
# ---------------------------------------------------------------------------
add_executable(pos_find src/pos_find.cpp)
target_compile_definitions(pos_find PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(pos_find PRIVATE advapi32)

# ---------------------------------------------------------------------------
#  entity_scan.exe  --  find every player by signature, no container needed
# ---------------------------------------------------------------------------
add_executable(entity_scan src/entity_scan.cpp)
target_compile_definitions(entity_scan PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(entity_scan PRIVATE advapi32)

# ---------------------------------------------------------------------------
#  find_refs.exe  --  locate the player container by reverse reference
# ---------------------------------------------------------------------------
add_executable(find_refs src/find_refs.cpp)
target_compile_definitions(find_refs PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(find_refs PRIVATE advapi32)

# ---------------------------------------------------------------------------
#  heap_mgr.exe  --  reach the player list through the heap manager
# ---------------------------------------------------------------------------
add_executable(heap_mgr src/heap_mgr.cpp)
target_compile_definitions(heap_mgr PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(heap_mgr PRIVATE advapi32)

# ---------------------------------------------------------------------------
#  kernel_payload.dll  --  position-independent kernel shellcode
#
#  Built as an ordinary DLL so the mapper can LoadLibrary it and read the
#  .vexstub / .payload section table, but with no CRT, no default libs and no
#  SEH.  The two custom sections hold the hijack stub and the manual mapper;
#  because they carry no absolute relocations, the mapper can copy each into
#  kernel memory at any address.
# ---------------------------------------------------------------------------
add_library(kernel_payload SHARED
    mapper/payload/kernel_payload.c
)

target_compile_definitions(kernel_payload PRIVATE UNICODE _UNICODE)

# Freestanding: no CRT, no default libs.  /Zl stops the compiler emitting a
# default-library directive, which is what drags in a libc memset reference and
# makes the link fail with LNK2019 when there is no CRT to satisfy it.
target_compile_options(kernel_payload PRIVATE
    /GS-
    /Gs999999
    /Gy
    /Oi-
    /GR-
    /EHs-c-
    /Zl
)

# Both custom sections must survive the linker: nothing in the DLL calls the
# stub or the payload entry, so a dead-stripping pass would drop them.  /INCLUDE
# is a LINKER option -- it has to go in target_link_options, not
# target_link_libraries, or CMake treats it as an input file and the link fails
# with LNK1181.
target_link_options(kernel_payload PRIVATE
    /NOENTRY
    /OPT:REF
    /OPT:ICF
    /DYNAMICBASE:NO
    "/INCLUDE:vex_boot_stub"
    "/INCLUDE:vex_payload_entry"
)

target_link_libraries(kernel_payload PRIVATE)

set_target_properties(kernel_payload PROPERTIES
    PREFIX ""
    OUTPUT_NAME "kernel_payload"
    SUFFIX ".dll"
)