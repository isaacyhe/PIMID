##
## CMakeLists.txt for ZSim QEMU plugin and trace driver builds
##
## Builds:
##   1. libpimid_trace.so  - Lightweight QEMU TCG plugin for trace generation (pure C)
##   2. libzsim_qemu.so    - QEMU TCG plugin with full ZSim simulation core (C++)
##   3. libzsim_trace.a    - Static library for trace-driven ZSim simulation
##
## These are QEMU plugins loaded via:
##   qemu-x86_64 -plugin libpimid_trace.so,output=file.pimtrace -- workload
##   qemu-x86_64 -plugin libzsim_qemu.so,cfg=zsim.cfg,out=/tmp/out -- workload
## The trace driver library is linked into pimid and called via zsim_trace_run().
##

cmake_minimum_required(VERSION 3.15)
project(zsim_qemu_plugins LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# QEMU plugin include directory (vendored header)
set(QEMU_PLUGIN_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ============================================================
# Target 1: libpimid_trace.so (lightweight trace generation)
# ============================================================
# Pure C, no ZSim dependency. Generates PIMID binary trace files.

add_library(pimid_trace SHARED
    src/qemu_trace_plugin.c
)

target_include_directories(pimid_trace PRIVATE
    ${QEMU_PLUGIN_INCLUDE}
)

target_compile_options(pimid_trace PRIVATE
    -fvisibility=hidden
    -Wall -Wextra -O2
)

target_link_libraries(pimid_trace PRIVATE pthread)

# ============================================================
# Target 2: libzsim_qemu.so (full ZSim simulation in QEMU)
# ============================================================
# C++, links all Pin-independent ZSim simulation core files.

# ZSim simulation core sources (Pin-independent)
set(ZSIM_SIM_SRCS
    # Core simulation
    src/cache.cpp
    src/cache_arrays.cpp
    src/coherence_ctrls.cpp
    src/config.cpp
    src/contention_sim.cpp
    src/core_recorder.cpp
    src/galloc.cpp
    src/hash.cpp
    src/init.cpp
    src/log.cpp
    src/mem_ctrls.cpp
    src/memory_hierarchy.cpp
    src/network.cpp

    # Cores (1.12.1: simple_core.cpp retired and deleted, ticket #114)
    src/in_order_core.cpp
    src/timing_cache.cpp
    src/timing_event.cpp
    src/ooo_core.cpp
    src/ooo_core_recorder.cpp
    src/null_core.cpp
    src/alu_core.cpp

    # Infrastructure
    src/prefetcher.cpp
    src/proc_stats.cpp
    src/process_stats.cpp
    src/process_tree.cpp
    src/scheduler.cpp
    src/stats_filter.cpp
    src/text_stats.cpp
    src/hdf5_stats.cpp
    src/trace_driver.cpp
    src/tracing_cache.cpp
    src/utility_monitor.cpp
    src/partition_mapper.cpp
    src/access_tracing.cpp
    src/debug_zsim.cpp
    src/lookahead.cpp
    src/monitor.cpp

    # Memory controllers
    src/ramulator_mem_ctrl.cpp
    src/dram_epoch_replay.cpp   # 1.11.98 (ruling 4 (c))

    # QEMU-specific replacements
    src/decoder_simple.cpp      # replaces decoder.cpp (no XED)
    src/qemu_zsim_plugin.cpp    # replaces zsim.cpp (QEMU frontend)
    src/qemu_stubs.cpp          # stubs for virt/syscall_name (no Pin)
)

# Check for libconfig
find_path(LIBCONFIG_INCLUDE_DIR libconfig.h++)
find_library(LIBCONFIG_LIBRARY NAMES config++)

add_library(zsim_qemu SHARED ${ZSIM_SIM_SRCS})

target_compile_definitions(zsim_qemu PRIVATE
    ZSIM_USE_QEMU
)

# HDF5: use CMake-discovered libraries (handles Debian/Ubuntu's libhdf5_serial
# naming, where a bare -lhdf5 does not exist). The parent project already ran
# find_package(HDF5 COMPONENTS C HL); re-run here if built standalone or not
# found -- zsim REQUIRES HDF5 (hdf5_stats.cpp).
if(NOT HDF5_FOUND)
    find_package(HDF5 REQUIRED COMPONENTS C HL)
endif()

target_include_directories(zsim_qemu PRIVATE
    src
    ${QEMU_PLUGIN_INCLUDE}
    ${HDF5_INCLUDE_DIRS}
)

if(LIBCONFIG_INCLUDE_DIR)
    target_include_directories(zsim_qemu PRIVATE ${LIBCONFIG_INCLUDE_DIR})
endif()

target_compile_options(zsim_qemu PRIVATE
    -fvisibility=hidden
    -Wall -Wextra -O2
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

# dlmalloc's DEBUG mode assertions are incompatible with the shared-memory
# allocator (create_mspace_with_base fails top-chunk checks).  Ensure
# galloc.cpp is always compiled without the DEBUG macro, even in Debug builds.
set_source_files_properties(src/galloc.cpp PROPERTIES COMPILE_FLAGS "-UDEBUG")

target_link_libraries(zsim_qemu PRIVATE
    pthread
    rt
    dl
    elf
    z
    ${HDF5_LIBRARIES}
    ${HDF5_HL_LIBRARIES}
)

# Link Garnet NoC library (if available from parent project)
if(TARGET garnet)
    target_link_libraries(zsim_qemu PRIVATE garnet)
    target_compile_definitions(zsim_qemu PRIVATE HAVE_GARNET)
endif()

# Link Ramulator2 DRAM simulator (if available from parent project)
if(TARGET ramulator)
    target_link_libraries(zsim_qemu PRIVATE ramulator)
    target_compile_definitions(zsim_qemu PRIVATE _WITH_RAMULATOR_)
    set_source_files_properties(src/ramulator_mem_ctrl.cpp PROPERTIES
        COMPILE_FLAGS "-std=c++20")
    set_source_files_properties(src/dram_epoch_replay.cpp PROPERTIES
        COMPILE_FLAGS "-std=c++20")
    target_include_directories(zsim_qemu PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../ramulator/src)
endif()

if(LIBCONFIG_LIBRARY)
    target_link_libraries(zsim_qemu PRIVATE ${LIBCONFIG_LIBRARY})
else()
    target_link_libraries(zsim_qemu PRIVATE config++)
endif()

# ============================================================
# Target 3: zsim_trace (trace-driven ZSim simulation library)
# ============================================================
# Static library linked into pimid binary. Replays PIMID trace files
# through the full ZSim simulation hierarchy (cores, caches, MCs, Garnet NoC).
# Uses the same simulation core as zsim_qemu but without QEMU dependency.

# Collect sim sources WITHOUT qemu_zsim_plugin.cpp
set(ZSIM_SIM_SRCS_TRACE
    # Core simulation
    src/cache.cpp
    src/cache_arrays.cpp
    src/coherence_ctrls.cpp
    src/config.cpp
    src/contention_sim.cpp
    src/core_recorder.cpp
    src/galloc.cpp
    src/hash.cpp
    src/init.cpp
    src/log.cpp
    src/mem_ctrls.cpp
    src/memory_hierarchy.cpp
    src/network.cpp

    # Cores (1.12.1: simple_core.cpp retired and deleted, ticket #114)
    src/in_order_core.cpp
    src/timing_cache.cpp
    src/timing_event.cpp
    src/ooo_core.cpp
    src/ooo_core_recorder.cpp
    src/null_core.cpp
    src/alu_core.cpp

    # Infrastructure
    src/prefetcher.cpp
    src/proc_stats.cpp
    src/process_stats.cpp
    src/process_tree.cpp
    src/scheduler.cpp
    src/stats_filter.cpp
    src/text_stats.cpp
    src/hdf5_stats.cpp
    src/trace_driver.cpp
    src/tracing_cache.cpp
    src/utility_monitor.cpp
    src/partition_mapper.cpp
    src/access_tracing.cpp
    src/debug_zsim.cpp
    src/lookahead.cpp
    src/monitor.cpp

    # Memory controllers
    src/ramulator_mem_ctrl.cpp
    src/dram_epoch_replay.cpp   # 1.11.98 (ruling 4 (c))

    # Shared replacements (no Pin/XED)
    src/decoder_simple.cpp
    src/qemu_stubs.cpp

    # Trace driver entry point (replaces qemu_zsim_plugin.cpp)
    src/zsim_trace_driver.cpp

    # Synthetic traffic injection (network-only)
    src/synthetic_traffic.cpp
)

add_library(zsim_trace STATIC ${ZSIM_SIM_SRCS_TRACE})

target_compile_definitions(zsim_trace PUBLIC
    ZSIM_USE_QEMU
)

target_include_directories(zsim_trace PUBLIC
    src
    ${QEMU_PLUGIN_INCLUDE}
    ${HDF5_INCLUDE_DIRS}
)

if(LIBCONFIG_INCLUDE_DIR)
    target_include_directories(zsim_trace PUBLIC ${LIBCONFIG_INCLUDE_DIR})
endif()

target_compile_options(zsim_trace PRIVATE
    -Wall -Wextra -O2
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

# galloc.cpp needs -UDEBUG for zsim_trace too
set_source_files_properties(src/galloc.cpp PROPERTIES COMPILE_FLAGS "-UDEBUG")

target_link_libraries(zsim_trace PUBLIC
    pthread
    rt
    dl
    elf
    z
    ${HDF5_LIBRARIES}
    ${HDF5_HL_LIBRARIES}
)

# Link Garnet NoC library (if available from parent project)
if(TARGET garnet)
    target_link_libraries(zsim_trace PUBLIC garnet)
    target_compile_definitions(zsim_trace PUBLIC HAVE_GARNET)
endif()

# Link Ramulator2 DRAM simulator (if available from parent project)
if(TARGET ramulator)
    target_link_libraries(zsim_trace PUBLIC ramulator)
    target_compile_definitions(zsim_trace PUBLIC _WITH_RAMULATOR_)
    target_include_directories(zsim_trace PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../ramulator/src)
endif()

if(LIBCONFIG_LIBRARY)
    target_link_libraries(zsim_trace PUBLIC ${LIBCONFIG_LIBRARY})
else()
    target_link_libraries(zsim_trace PUBLIC config++)
endif()

# ============================================================
# Install
# ============================================================

install(TARGETS pimid_trace zsim_qemu
    LIBRARY DESTINATION lib/pimid/plugins
)

install(TARGETS zsim_trace
    ARCHIVE DESTINATION lib
)
