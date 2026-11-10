# Adds our co-op code to SoH's soh.exe without touching any SoH file.
#
# How: SoH is configured with -DCMAKE_PROJECT_soh_INCLUDE=<this file>, so CMake
# runs this right after SoH's `project(soh)`. At that point the soh target
# doesn't exist yet, so the real work is deferred to the end of soh/CMakeLists.txt.
#
# Our files are built as a separate object library with the same settings as
# soh, then just linked in. soh's own compile flags never change, so adding
# or editing co-op code doesn't trigger a rebuild of SoH's 1000+ files.
#
# Use scripts/build-soh.ps1 rather than calling this by hand.

get_filename_component(SOHT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

include(FetchContent)
FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.18
)
FetchContent_MakeAvailable(enet)

# Build id = our commit (+ "-dirty"). Only refreshed when CMake re-runs.
execute_process(COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${SOHT_ROOT}" OUTPUT_VARIABLE SOHT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git diff --quiet HEAD -- src cmake
    WORKING_DIRECTORY "${SOHT_ROOT}" RESULT_VARIABLE SOHT_DIRTY ERROR_QUIET)
if(NOT SOHT_COMMIT)
    set(SOHT_COMMIT "unknown")
endif()
set(SOHT_BUILD_ID "soht:${SOHT_COMMIT}")
if(SOHT_DIRTY)
    string(APPEND SOHT_BUILD_ID "-dirty")
endif()
configure_file("${SOHT_ROOT}/cmake/BuildId.h.in" "${CMAKE_BINARY_DIR}/soht/soh/Network/Coop/BuildId.h" @ONLY)
message(STATUS "SOH-Together: co-op code will be built in (${SOHT_BUILD_ID})")

function(soht_inject_coop)
    file(GLOB SOHT_SOURCES CONFIGURE_DEPENDS "${SOHT_ROOT}/src/soh/Network/Coop/*.cpp")
    add_library(soht_coop OBJECT ${SOHT_SOURCES})

    # Same includes, defines and flags as soh itself (these generator
    # expressions also pull in what soh gets from the libs it links).
    get_target_property(SOH_LINK_LIBS soh LINK_LIBRARIES)
    if(SOH_LINK_LIBS)
        # soh also links some raw object files, those belong to soh only
        list(FILTER SOH_LINK_LIBS EXCLUDE REGEX "TARGET_OBJECTS")
        target_link_libraries(soht_coop PRIVATE ${SOH_LINK_LIBS})
    endif()
    target_include_directories(soht_coop PRIVATE
        "${SOHT_ROOT}/src"
        "${CMAKE_BINARY_DIR}/soht"
        "${enet_SOURCE_DIR}/include"
        $<TARGET_PROPERTY:soh,INCLUDE_DIRECTORIES>)
    target_compile_definitions(soht_coop PRIVATE $<TARGET_PROPERTY:soh,COMPILE_DEFINITIONS>)
    target_compile_options(soht_coop PRIVATE $<TARGET_PROPERTY:soh,COMPILE_OPTIONS>)

    # Mixing static and dynamic C runtimes breaks the link (LNK2038), so copy soh's choice.
    get_target_property(SOH_RUNTIME soh MSVC_RUNTIME_LIBRARY)
    if(SOH_RUNTIME)
        set_target_properties(soht_coop enet PROPERTIES MSVC_RUNTIME_LIBRARY "${SOH_RUNTIME}")
    endif()
    if(MSVC)
        target_compile_options(enet PRIVATE /wd5287 /wd4996)
    endif()

    # Only link-time stuff on soh from here: no recompilation of SoH.
    target_sources(soh PRIVATE $<TARGET_OBJECTS:soht_coop>)
    add_dependencies(soh soht_coop)
    target_link_libraries(soh PRIVATE enet)
    if(WIN32)
        target_link_libraries(soh PRIVATE ws2_32 winmm)
    endif()
endfunction()

cmake_language(DEFER CALL soht_inject_coop)
