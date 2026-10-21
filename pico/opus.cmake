# libopus for the headphone audio (pico/headset_audio_pico.c): fixed point,
# CELT through OPUS_APPLICATION_RESTRICTED_LOWDELAY, no float analysis.
# From a local checkout (-DOPUS_DIR=...) or fetched from GitHub.
set(OPUS_VERSION v1.5.2)
if (NOT DEFINED OPUS_DIR)
    include(FetchContent)
    FetchContent_Declare(opus_src
            GIT_REPOSITORY https://github.com/xiph/opus.git
            GIT_TAG ${OPUS_VERSION}
            GIT_SHALLOW TRUE)
    FetchContent_GetProperties(opus_src)
    if (NOT opus_src_POPULATED)
        FetchContent_Populate(opus_src)
    endif()
    set(OPUS_DIR ${opus_src_SOURCE_DIR})
endif()

# Source lists from libopus's own makefiles.
function(opus_mk_list file var out)
    file(READ ${OPUS_DIR}/${file} txt)
    string(REPLACE "\\\n" " " txt "${txt}")
    string(REGEX MATCH "${var} = [^\n]*" line "${txt}")
    string(REPLACE "${var} = " "" line "${line}")
    separate_arguments(items UNIX_COMMAND "${line}")
    list(TRANSFORM items PREPEND ${OPUS_DIR}/)
    set(${out} ${items} PARENT_SCOPE)
endfunction()
opus_mk_list(celt_sources.mk CELT_SOURCES OPUS_CELT_SRC)
opus_mk_list(silk_sources.mk SILK_SOURCES OPUS_SILK_SRC)
opus_mk_list(silk_sources.mk SILK_SOURCES_FIXED OPUS_SILK_FIXED_SRC)
opus_mk_list(opus_sources.mk OPUS_SOURCES OPUS_MAIN_SRC)

add_library(opus STATIC ${OPUS_CELT_SRC} ${OPUS_SILK_SRC} ${OPUS_SILK_FIXED_SRC} ${OPUS_MAIN_SRC})
target_include_directories(opus PUBLIC ${OPUS_DIR}/include
        PRIVATE ${OPUS_DIR}/celt ${OPUS_DIR}/silk ${OPUS_DIR}/silk/fixed ${OPUS_DIR}/src ${OPUS_DIR})
# The scratch memory ("pseudostack") is one static buffer: only core 1 uses libopus.
target_compile_definitions(opus PRIVATE OPUS_BUILD FIXED_POINT DISABLE_FLOAT_API HAVE_LRINT HAVE_LRINTF
        NONTHREADSAFE_PSEUDOSTACK GLOBAL_STACK_SIZE=120000)
target_compile_options(opus PRIVATE -O2 -w)
target_link_libraries(opus PRIVATE pico_stdlib)

# Run the encoder's hot code from RAM. From flash it was 2.7 times slower in
# the firmware (about 25 ms per frame instead of 9.4) because core 0 keeps
# evicting it from the shared XIP cache. The SDK's linker script leaves files
# excluded from the flash .text in .data (RAM); add the CELT encode path.
# (SILK, linked but unused with RESTRICTED_LOWDELAY, stays in flash.)
# Extra files (as celt/celt_decoder) after `target` go to RAM too.
function(opus_ram_linker_script target)
    string(REGEX REPLACE "-.*" "" chip ${PICO_PLATFORM})   # rp2350-arm-s -> rp2350
    set(sdk_ld ${PICO_SDK_PATH}/src/rp2_common/pico_crt0/${chip}/memmap_default.ld)
    file(READ ${sdk_ld} ld)
    set(objs "")
    foreach (f celt/bands celt/celt celt/celt_encoder celt/celt_lpc celt/cwrs celt/entcode celt/entenc
            celt/kiss_fft celt/laplace celt/mathops celt/mdct celt/modes celt/pitch celt/quant_bands
            celt/rate celt/vq src/opus src/opus_encoder ${ARGN})
        get_filename_component(name ${f} NAME)
        string(APPEND objs " *libopus.a:${name}.c.o")
    endforeach()
    set(from "*(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:) .text*)")
    set(to "*(EXCLUDE_FILE(*libgcc.a: *libc.a:*lib_a-mem*.o *libm.a:${objs}) .text*)")
    string(FIND "${ld}" "${from}" at)
    if (at EQUAL -1)
        message(FATAL_ERROR "opus.cmake: the SDK's memmap_default.ld changed; update opus_ram_linker_script")
    endif()
    string(REPLACE "${from}" "${to}" ld "${ld}")
    set(out ${CMAKE_CURRENT_BINARY_DIR}/memmap_opus_ram.ld)
    file(WRITE ${out} "${ld}")
    pico_set_linker_script(${target} ${out})
endfunction()
