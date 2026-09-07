cmake_minimum_required(VERSION 3.20)

project(DualBoy VERSION 0.1.0 LANGUAGES C)

include(CTest)

option(DUALBOY_WARNINGS_AS_ERRORS "Treat DualBoy warnings as errors" ON)
option(DUALBOY_ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(DUALBOY_ENABLE_ASAN AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    # Apply sanitizers at directory scope so code in static archives (including
    # both emulator adapters) is instrumented, not only the final shared object.
    add_compile_options(
        -fno-omit-frame-pointer -fsanitize=address,undefined
    )
    add_link_options(
        -fno-omit-frame-pointer -fsanitize=address,undefined
    )
endif()

add_library(dualboy_frontend STATIC
    src/frontend/content.c
    src/frontend/compositor.c
    src/frontend/persistence.c
    src/frontend/playlist.c
    src/frontend/save_manager.c
    src/frontend/session.c
    src/frontend/state.c
)

target_include_directories(dualboy_frontend PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

set(SAMEBOY_CORE_SOURCES
    third_party/sameboy/Core/gb.c
    third_party/sameboy/Core/sgb.c
    third_party/sameboy/Core/apu.c
    third_party/sameboy/Core/memory.c
    third_party/sameboy/Core/mbc.c
    third_party/sameboy/Core/timing.c
    third_party/sameboy/Core/display.c
    third_party/sameboy/Core/camera.c
    third_party/sameboy/Core/sm83_cpu.c
    third_party/sameboy/Core/joypad.c
    third_party/sameboy/Core/save_state.c
    third_party/sameboy/Core/random.c
    third_party/sameboy/Core/rumble.c
)

add_library(dualboy_sameboy_upstream STATIC ${SAMEBOY_CORE_SOURCES})
target_include_directories(dualboy_sameboy_upstream PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sameboy
)
target_compile_definitions(dualboy_sameboy_upstream PRIVATE
    _GNU_SOURCE
    _USE_MATH_DEFINES
    GB_INTERNAL
    GB_DISABLE_TIMEKEEPING
    GB_DISABLE_REWIND
    GB_DISABLE_DEBUGGER
    GB_DISABLE_CHEATS
    GB_VERSION="1.0.3"
)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dualboy_sameboy_upstream PRIVATE -Wno-multichar)
endif()

add_library(dualboy_sameboy_adapter STATIC
    src/engines/sameboy/sameboy_adapter.c
    src/engines/sameboy/embedded_boot_roms.c
)
target_include_directories(dualboy_sameboy_adapter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sameboy
)
target_compile_definitions(dualboy_sameboy_adapter PRIVATE
    _GNU_SOURCE
    _USE_MATH_DEFINES
    GB_INTERNAL
    GB_DISABLE_TIMEKEEPING
    GB_DISABLE_REWIND
    GB_DISABLE_DEBUGGER
    GB_DISABLE_CHEATS
    GB_VERSION="1.0.3"
)
target_link_libraries(dualboy_sameboy_adapter PUBLIC dualboy_sameboy_upstream)
if(UNIX)
    target_link_libraries(dualboy_sameboy_adapter PUBLIC m)
endif()

function(dualboy_add_mgba_upstream)
    set(BINARY_NAME dualboy_mgba_upstream)
    set(M_CORE_GBA ON)
    set(M_CORE_GB OFF)
    set(MINIMAL_CORE ON)
    set(ENABLE_EXTRA OFF)
    set(ENABLE_SCRIPTING OFF)
    add_subdirectory(
        third_party/mgba
        ${CMAKE_CURRENT_BINARY_DIR}/third_party/mgba
        EXCLUDE_FROM_ALL
    )
endfunction()
dualboy_add_mgba_upstream()

target_compile_definitions(dualboy_mgba_upstream PUBLIC
    M_CORE_GBA
    MINIMAL_CORE=2
    DISABLE_THREADING
    ENABLE_VFS
)

add_library(dualboy_mgba_adapter STATIC
    src/engines/mgba/mgba_adapter.c
    src/engines/mgba/mgba_lockstep.c
)
target_include_directories(dualboy_mgba_adapter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_include_directories(dualboy_mgba_adapter SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mgba/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mgba/src
)
target_compile_definitions(dualboy_mgba_adapter PRIVATE
    M_CORE_GBA
    MINIMAL_CORE=2
    DISABLE_THREADING
    ENABLE_DIRECTORIES
    ENABLE_VFS
)
target_link_libraries(dualboy_mgba_adapter PUBLIC dualboy_mgba_upstream)
if(UNIX)
    target_link_libraries(dualboy_mgba_adapter PUBLIC m)
endif()

add_library(dualboy_libretro SHARED
    src/libretro/libretro_core.c
    src/libretro/options.c
)

target_include_directories(dualboy_libretro PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libretro-common/include
)

target_link_libraries(dualboy_libretro PRIVATE
    dualboy_frontend
    dualboy_sameboy_adapter
    dualboy_mgba_adapter
)

target_compile_definitions(dualboy_libretro PRIVATE
    DUALBOY_VERSION="${PROJECT_VERSION}"
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dualboy_frontend PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(dualboy_libretro PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(dualboy_sameboy_adapter PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(dualboy_mgba_adapter PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    if(DUALBOY_WARNINGS_AS_ERRORS)
        target_compile_options(dualboy_frontend PRIVATE -Werror)
        target_compile_options(dualboy_libretro PRIVATE -Werror)
        target_compile_options(dualboy_sameboy_adapter PRIVATE -Werror)
        target_compile_options(dualboy_mgba_adapter PRIVATE -Werror)
    endif()
endif()

set_target_properties(dualboy_libretro PROPERTIES
    PREFIX ""
    OUTPUT_NAME "dualboy_libretro"
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

if(UNIX AND NOT APPLE)
    target_link_options(dualboy_libretro PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/libretro/link.T"
        "LINKER:--no-undefined"
    )
endif()

if(BUILD_TESTING)
    add_executable(frontend_test tests/frontend_test.c)
    target_link_libraries(frontend_test PRIVATE dualboy_frontend)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(frontend_test PRIVATE -Wall -Wextra -Wpedantic)
        if(DUALBOY_WARNINGS_AS_ERRORS)
            target_compile_options(frontend_test PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME frontend_unit COMMAND frontend_test)

    add_executable(session_test tests/session_test.c)
    target_link_libraries(session_test PRIVATE dualboy_frontend)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(session_test PRIVATE -Wall -Wextra -Wpedantic)
        if(DUALBOY_WARNINGS_AS_ERRORS)
            target_compile_options(session_test PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME session_unit COMMAND session_test)

    add_executable(state_test tests/state_test.c)
    target_link_libraries(state_test PRIVATE dualboy_frontend)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(state_test PRIVATE -Wall -Wextra -Wpedantic)
        if(DUALBOY_WARNINGS_AS_ERRORS)
            target_compile_options(state_test PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME state_unit COMMAND state_test)

    add_executable(persistence_test tests/persistence_test.c)
    target_link_libraries(persistence_test PRIVATE dualboy_frontend)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(persistence_test PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        )
        if(DUALBOY_WARNINGS_AS_ERRORS)
            target_compile_options(persistence_test PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME persistence_unit COMMAND persistence_test)

    add_executable(save_manager_test tests/save_manager_test.c)
    target_link_libraries(save_manager_test PRIVATE dualboy_frontend)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(save_manager_test PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        )
        if(DUALBOY_WARNINGS_AS_ERRORS)
            target_compile_options(save_manager_test PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME save_manager_unit COMMAND save_manager_test)

    add_executable(sameboy_adapter_test tests/sameboy_adapter_test.c)
    target_link_libraries(sameboy_adapter_test PRIVATE
        dualboy_sameboy_adapter
        dualboy_frontend
    )
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(sameboy_adapter_test PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        )
        if(DUALBOY_WARNINGS_AS_ERRORS)
            target_compile_options(sameboy_adapter_test PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME sameboy_adapter_unit COMMAND sameboy_adapter_test)

    add_executable(mgba_adapter_test tests/mgba_adapter_test.c)
    target_link_libraries(mgba_adapter_test PRIVATE
        dualboy_mgba_adapter
        dualboy_frontend
    )
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(mgba_adapter_test PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        )
        if(DUALBOY_WARNINGS_AS_ERRORS)
            target_compile_options(mgba_adapter_test PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME mgba_adapter_unit COMMAND mgba_adapter_test)

    add_executable(libretro_smoke tests/libretro_smoke.c)
    target_include_directories(libretro_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libretro-common/include
    )
    target_link_libraries(libretro_smoke PRIVATE ${CMAKE_DL_LIBS})
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(libretro_smoke PRIVATE -Wall -Wextra -Wpedantic)
        if(DUALBOY_WARNINGS_AS_ERRORS)
            target_compile_options(libretro_smoke PRIVATE -Werror)
        endif()
    endif()
    add_test(NAME libretro_abi_smoke
        COMMAND libretro_smoke $<TARGET_FILE:dualboy_libretro>
    )
endif()

install(TARGETS dualboy_libretro LIBRARY DESTINATION lib/libretro)
install(FILES dualboy_libretro.info DESTINATION share/libretro/info)
