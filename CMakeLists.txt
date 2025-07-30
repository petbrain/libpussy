project(libpussy)

#set(CMAKE_C_COMPILER clang)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -pedantic -Werror -Wno-unused-parameter)

if(DEFINED ENV{DEBUG})
    add_compile_options(-g)
else()
    add_compile_options(-O3)
endif()

add_library(pussy STATIC
    src/allocator.c
    src/allocator_pet.c
    src/allocator_debug.c
    src/allocator_stdlib.c
    src/arena.c
    src/dump_bitmap.c
    src/dump_hex.c
    src/fsb_arena.c
    src/mmarray.c
    src/ringbuffer_base.c
    src/ringbuffer_sync.c
    src/sync_event.c
    src/timespec.c
)

target_include_directories(pussy PUBLIC . include libpussy)

# common definitions

#set(common_defs_targets pussy test_pussy)
set(common_defs_targets pussy)

foreach(TARGET ${common_defs_targets})

    if(DEFINED ENV{DEBUG})
        target_compile_definitions(${TARGET} PUBLIC DEBUG)
    endif()

endforeach(TARGET)
