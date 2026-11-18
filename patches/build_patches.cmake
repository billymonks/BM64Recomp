# Builds patches.elf without make. Invoked as:
#   cmake -DCC=<clang> -DLD=<ld.lld> -P build_patches.cmake   (from the patches directory)
# Flags must stay in sync with patches/Makefile.

if(NOT CC)
    set(CC clang)
endif()
if(NOT LD)
    set(LD ld.lld)
endif()

set(CFLAGS -target mips -mips2 -mabi=32 -O0 -G0 -mno-abicalls -mno-odd-spreg -mno-check-zero-division
    -fomit-frame-pointer -ffast-math -fno-unsafe-math-optimizations -fno-builtin-memset
    -Wall -Wextra -Wno-incompatible-library-redeclaration -Wno-unused-parameter -Wno-unknown-pragmas -Wno-unused-variable
    -Wno-unused-but-set-variable -Wno-missing-braces -Wno-unsupported-floating-point-opt -Wno-switch -D_MIPS_SZLONG=32)
set(CPPFLAGS -nostdinc -DF3DEX_GBI -D_LANGUAGE_C -DMIPS -I dummy_headers -I ../lib/sf64/include -I ../lib/sf64/include/libultra
    -I ../lib/sf64/src -I ../lib/rt64/include -I ../lib/N64ModernRuntime/ultramodern/include
    -I ../lib/N64ModernRuntime/ultramodern/include/ultramodern)
set(LDFLAGS -nostdlib -T patches.ld -T syms.ld -Map patches.map --unresolved-symbols=ignore-all --emit-relocs)

set(DIR ${CMAKE_CURRENT_LIST_DIR})
file(GLOB C_SRCS RELATIVE ${DIR} ${DIR}/*.c)
set(C_OBJS)
set(NEED_LINK FALSE)
if(NOT EXISTS ${DIR}/patches.elf)
    set(NEED_LINK TRUE)
endif()

foreach(src ${C_SRCS})
    string(REGEX REPLACE "\\.c$" ".o" obj ${src})
    string(REGEX REPLACE "\\.c$" ".d" dep ${src})
    list(APPEND C_OBJS ${obj})

    set(rebuild FALSE)
    if(NOT EXISTS ${DIR}/${obj} OR ${DIR}/${src} IS_NEWER_THAN ${DIR}/${obj})
        set(rebuild TRUE)
    elseif(EXISTS ${DIR}/${dep})
        file(READ ${DIR}/${dep} dep_text)
        string(REGEX REPLACE "\\\\\n" " " dep_text "${dep_text}")
        string(REGEX REPLACE "^[^:]*:" "" dep_text "${dep_text}")
        separate_arguments(dep_files UNIX_COMMAND "${dep_text}")
        foreach(d ${dep_files})
            get_filename_component(d ${d} ABSOLUTE BASE_DIR ${DIR})
            if(EXISTS ${d} AND ${d} IS_NEWER_THAN ${DIR}/${obj})
                set(rebuild TRUE)
                break()
            endif()
        endforeach()
    endif()

    if(rebuild)
        message(STATUS "patches: compiling ${src}")
        execute_process(COMMAND ${CC} ${CFLAGS} ${CPPFLAGS} ${src} -MMD -MF ${dep} -c -o ${obj} WORKING_DIRECTORY ${DIR}
                        RESULT_VARIABLE res)
        if(NOT res EQUAL 0)
            message(FATAL_ERROR "patches: failed to compile ${src}")
        endif()
        set(NEED_LINK TRUE)
    endif()
endforeach()

if(${DIR}/patches.ld IS_NEWER_THAN ${DIR}/patches.elf OR ${DIR}/syms.ld IS_NEWER_THAN ${DIR}/patches.elf)
    set(NEED_LINK TRUE)
endif()

if(NEED_LINK)
    message(STATUS "patches: linking patches.elf")
    execute_process(COMMAND ${LD} ${C_OBJS} ${LDFLAGS} -o patches.elf WORKING_DIRECTORY ${DIR} RESULT_VARIABLE res)
    if(NOT res EQUAL 0)
        message(FATAL_ERROR "patches: failed to link patches.elf")
    endif()
endif()
