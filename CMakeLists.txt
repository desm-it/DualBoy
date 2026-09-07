cmake_minimum_required(VERSION 3.20)

project(DualBoy VERSION 0.1.0 LANGUAGES C)

include(CTest)

option(DUALBOY_WARNINGS_AS_ERRORS "Treat DualBoy warnings as errors" ON)
option(DUALBOY_ENABLE_ASAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(dualboy_libretro SHARED
    src/libretro/libretro_core.c
)

target_include_directories(dualboy_libretro PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libretro-common/include
)

target_compile_definitions(dualboy_libretro PRIVATE
    DUALBOY_VERSION="${PROJECT_VERSION}"
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dualboy_libretro PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    if(DUALBOY_WARNINGS_AS_ERRORS)
        target_compile_options(dualboy_libretro PRIVATE -Werror)
    endif()
endif()

if(DUALBOY_ENABLE_ASAN AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(dualboy_libretro PRIVATE
        -fno-omit-frame-pointer -fsanitize=address,undefined
    )
    target_link_options(dualboy_libretro PRIVATE
        -fno-omit-frame-pointer -fsanitize=address,undefined
    )
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
    if(DUALBOY_ENABLE_ASAN AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(libretro_smoke PRIVATE
            -fno-omit-frame-pointer -fsanitize=address,undefined
        )
        target_link_options(libretro_smoke PRIVATE
            -fno-omit-frame-pointer -fsanitize=address,undefined
        )
    endif()
    add_test(NAME libretro_abi_smoke
        COMMAND libretro_smoke $<TARGET_FILE:dualboy_libretro>
    )
endif()

install(TARGETS dualboy_libretro LIBRARY DESTINATION lib/libretro)
install(FILES dualboy_libretro.info DESTINATION share/libretro/info)
