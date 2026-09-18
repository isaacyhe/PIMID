cmake_minimum_required(VERSION 3.15)
project(PIMID VERSION 1.11.66 LANGUAGES CXX)

# The reported version is taken FROM the project version, so the two cannot
# drift apart again (see release 1.10.2).
add_compile_definitions(PIMID_VERSION="${PROJECT_VERSION}")

# C++17 standard required
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
set(CMAKE_CXX_FLAGS "-Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Include directories - NEW STRUCTURE
# Note: CACTI 7.0 headers are added per-target to avoid naming conflicts
include_directories(
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src                 # so "memory/x.h" finds src/memory/
    ${PROJECT_SOURCE_DIR}/external
    ${PROJECT_SOURCE_DIR}/external/ramulator/src
    ${PROJECT_SOURCE_DIR}/external/nvsim      # Namespaced NVSim (avoids CACTI collision)
    ${PROJECT_SOURCE_DIR}/external/mcpat
)

# Find required packages
find_package(Threads REQUIRED)

# Optional: Boost for enhanced functionality
find_package(Boost COMPONENTS system filesystem)
if(Boost_FOUND)
    add_definitions(-DHAVE_BOOST)
endif()

# Optional: yaml-cpp for configuration parsing
find_package(yaml-cpp)
if(yaml-cpp_FOUND)
    add_definitions(-DHAVE_YAML_CPP)
endif()

# Optional: HDF5 for real per-access trace conversion from ZSim
find_package(HDF5 COMPONENTS C HL)
if(HDF5_FOUND)
    add_definitions(-DHAVE_HDF5)
endif()

# Build external models
message(STATUS "Building external models...")

# ── Mandatory external models ──
# All five models ship with PIMID and MUST be present.

# Ramulator2 for DRAM modeling
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/external/ramulator/CMakeLists.txt")
    message(FATAL_ERROR "Ramulator2 not found at external/ramulator/. "
        "All external models are mandatory — check your git submodules.")
endif()
message(STATUS "  - Ramulator2 DRAM simulator")
set(OLD_CXX_STANDARD ${CMAKE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD 20)
add_subdirectory(external/ramulator)
set(CMAKE_CXX_STANDARD ${OLD_CXX_STANDARD})
add_definitions(-DHAVE_RAMULATOR)
set(HAVE_RAMULATOR ON)

# CACTI 7.0 for SRAM/DRAM array characterization and McPAT power modeling
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/external/cacti/CMakeLists.txt")
    message(FATAL_ERROR "CACTI 7.0 not found at external/cacti/. "
        "All external models are mandatory — check your git submodules.")
endif()
message(STATUS "  - CACTI 7.0 SRAM/DRAM array analyzer")
add_subdirectory(external/cacti)
add_definitions(-DHAVE_CACTI)
add_definitions(-DHAVE_CACTI7)
set(HAVE_CACTI ON)
set(HAVE_CACTI7 ON)

# NVSim for NVM modeling (namespaced to avoid CACTI symbol collision)
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/external/nvsim/CMakeLists.txt")
    message(FATAL_ERROR "NVSim not found at external/nvsim/. "
        "All external models are mandatory — check your git submodules.")
endif()
message(STATUS "  - NVSim NVM analyzer (namespaced)")
add_subdirectory(external/nvsim)
add_definitions(-DHAVE_NVSIM)
set(HAVE_NVSIM ON)

# McPAT for power modeling (using CACTI 7.0)
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/external/mcpat/CMakeLists.txt")
    message(FATAL_ERROR "McPAT not found at external/mcpat/. "
        "All external models are mandatory — check your git submodules.")
endif()
message(STATUS "  - McPAT power analyzer (using CACTI 7)")
add_subdirectory(external/mcpat)
add_definitions(-DHAVE_MCPAT)
set(HAVE_MCPAT ON)

# Garnet NoC simulator (extracted from gem5)
if(NOT EXISTS "${PROJECT_SOURCE_DIR}/external/garnet/CMakeLists.txt")
    message(FATAL_ERROR "Garnet not found at external/garnet/. "
        "All external models are mandatory — check your git submodules.")
endif()
message(STATUS "  - Garnet NoC simulator (extracted from gem5)")
add_subdirectory(external/garnet)
set(HAVE_GARNET ON)

# Options
option(BUILD_PLUGINS "Build plugin system" ON)
option(BUILD_TOOLS "Build configuration tools" ON)
option(BUILD_EXAMPLES "Build example plugins" ON)
option(PIMID_USE_QEMU "Build QEMU TCG plugins (libpimid_trace.so, libzsim_qemu.so)" ON)

# QEMU TCG plugin builds (trace generation + live ZSim execution)
if(PIMID_USE_QEMU AND EXISTS "${PROJECT_SOURCE_DIR}/external/zsim/CMakeLists.txt")
    message(STATUS "  - QEMU plugins: YES (libpimid_trace.so, libzsim_qemu.so)")
    add_subdirectory(external/zsim)
    set(HAVE_QEMU_PLUGINS ON)
endif()

# Source files - NEW PATHS
set(PIMID_SOURCES
    src/util/cache_warehouse.cpp
    # Common
    src/common/simulation_engine.cpp
    src/common/event_queue.cpp
    src/common/config_parser.cpp
    src/config/config_parser.cpp
    src/config/config_manager.cpp
    src/config/config_schema.cpp
    src/config/config_validator.cpp
    src/config/network_config_parser.cpp

    # Trace infrastructure
    src/trace/trace_writer.cpp
    src/trace/trace_reader.cpp

    # Memory models - NEW PATHS
    src/memory/memory_model.cpp
    src/memory/dram_model.cpp
    src/memory/sram_model.cpp
    # 1.11.57 (latent D051): src/memory/nvm_model.cpp DELETED -- NVMModel had
    # no constructor anywhere in the tree (the factory routes STT_MRAM/PCM/
    # ReRAM to their own models) and its 638 lines were a standing re-entry
    # point for the literals 1.11.24 removed: a "// Ultimate fallback:
    # hard-coded defaults" block of 0.3/1.0/0.5 nJ reads and 15/12/10 mm^2
    # areas, sitting directly under a comment stating the refusal policy.
    src/memory/sttmram_model.cpp
    src/memory/pcm_model.cpp
    src/memory/reram_model.cpp
    src/memory/ramulator_wrapper.cpp
    src/memory/cacti_wrapper.cpp
    src/memory/nvsim_wrapper.cpp

    # PIM extensions
    src/memory/pim_bandwidth_tracker.cpp
    src/memory/internal_dram_network.cpp
    src/memory/internal_memory_network.cpp
    src/memory/pim_controller_plugin.cpp

    # DRAM architecture specifications
    # 1.11.59 (C016-class): src/memory/dram_architecture.{h,cpp} DELETED --
    # the legacy v1 DRAMArchitecture object. It was compiled into pimid_lib
    # but had no caller anywhere: a tree-wide search (all files, build/ and
    # external/ excluded) found the header included only by its own .cpp, and
    # none of DRAMArchitectureFactory, createDDR4_2400, createDDR5_4800,
    # createHBM2, createHBM3 or DRAMArchitecture::printSummary referenced
    # outside those two files. Everything live goes through
    # src/memory/dram_architecture_v2.h's create*_Verified() creators
    # (dram_model.cpp, ramulator_wrapper.cpp, pe_placement.h).
    #
    # It was deleted rather than repaired because it carried the same drifted
    # HBM access-time literals that 1.11.57 had just corrected in the v2
    # object: HBM2 subarray/bank 25.0/37.5 ns and HBM3 20.0/30.0 ns, against
    # the sums those fields declare themselves to be (subarray = tRCD + tCAS,
    # bank = tRP + tRCD + tCAS). The numbers were self-consistent when
    # written -- 25.0 is 12.5+12.5 and 37.5 is 3 x 12.5, HBM2's pre-1.1.1
    # arithmetic exactly -- and went wrong at release 1.1.1, which raised
    # HBM2 tRCD/tCAS from 12.5 to the JESD235C spec minimum 16.0 and HBM3
    # tRCD/tCAS from 10.0 to the JESD238 minimum 16.0 in the v2 object and
    # left the derived literals here untouched. Against the corrected timing
    # they read 22% (HBM2 subarray), 16% (HBM2 bank), 37% (HBM3 subarray) and
    # 29% (HBM3 bank) low. That was invisible because nothing called the
    # accessors: a wrong number that no code path reads produces no wrong
    # result and no failing check, so it sat here for 58 releases waiting for
    # someone to wire the class up and inherit the drift silently. Deleting
    # it removes the re-entry point; the v2 object derives both sums from
    # tRCD/tCAS/tRP at construction and cannot drift again. Recoverable from
    # git history if a v1 consumer is ever really wanted.

    # Network - NEW PATHS
    src/network/network_model.cpp
    src/network/garnet_detailed.cpp
    src/network/garnet_wrapper.cpp

    # Power - NEW PATHS
    src/power/power_model.cpp
    src/power/mcpat_model.cpp
    src/power/mcpat_wrapper.cpp
    src/power/cacti_io_wrapper.cpp   # 1.11.40: CACTI-IO harness (audit E19/link scope)
    src/power/power_model_manager.cpp

    # Model registry - NEW PATH
    src/model_registry.cpp

    # Address translation
    src/address_translation/pe_placement.cpp
    src/address_translation/address_translator.cpp
    src/address_translation/pe_statistics.cpp

    # Scheduler
    src/scheduler/scheduler.cpp
    src/scheduler/nearest_scheduler.cpp
    src/scheduler/roundrobin_scheduler.cpp
    src/scheduler/loadbalanced_scheduler.cpp

    # Instrumentation (PIMID API for workloads)
    src/instrumentation/pimid_api.cpp

)

# Plugin system sources
set(PIMID_PLUGIN_SOURCES
    src/plugin/plugin_interface.cpp
    src/plugin/memory_model_plugin.cpp
    src/plugin/scheduler_plugin.cpp
)

# PIMID core library
add_library(pimid_lib STATIC ${PIMID_SOURCES})
target_link_libraries(pimid_lib
    Threads::Threads
    ${CMAKE_DL_LIBS}
    yaml-cpp
)

if(Boost_FOUND)
    target_link_libraries(pimid_lib Boost::system Boost::filesystem)
endif()

if(HDF5_FOUND)
    target_link_libraries(pimid_lib ${HDF5_LIBRARIES} ${HDF5_HL_LIBRARIES})
    target_include_directories(pimid_lib PRIVATE ${HDF5_INCLUDE_DIRS})
endif()

# Link external models
if(HAVE_RAMULATOR)
    target_link_libraries(pimid_lib ramulator)
endif()

if(HAVE_CACTI7)
    # Link CACTI 7.0 for subarray characterization
    target_link_libraries(pimid_lib cacti7)
    # Add CACTI 7.0 include directories specifically for pimid_lib
    target_include_directories(pimid_lib PRIVATE ${PROJECT_SOURCE_DIR}/external/cacti)
    # Pass CACTI source directory so CACTIWrapper can find tech_params at runtime
    target_compile_definitions(pimid_lib PRIVATE
        CACTI_DATA_DIR="${PROJECT_SOURCE_DIR}/external/cacti")
endif()

if(HAVE_NVSIM)
    target_link_libraries(pimid_lib nvsim_ns)  # Namespaced NVSim library
    target_compile_definitions(pimid_lib PRIVATE
        NVSIM_DATA_DIR="${PROJECT_SOURCE_DIR}/external/nvsim")
endif()

if(HAVE_MCPAT)
    target_link_libraries(pimid_lib mcpat)
endif()

# Garnet NoC simulator (extracted from gem5)
if(HAVE_GARNET)
    target_link_libraries(pimid_lib garnet)
    target_include_directories(pimid_lib PRIVATE
        ${PROJECT_SOURCE_DIR}/external/garnet/src
        ${PROJECT_SOURCE_DIR}/external/garnet/include
        ${PROJECT_SOURCE_DIR}/external/garnet/include/gem5_compat
    )
    add_definitions(-DHAVE_GARNET)
endif()

# Plugin system library
if(BUILD_PLUGINS)
    add_library(pimid_plugin SHARED ${PIMID_PLUGIN_SOURCES})
    target_link_libraries(pimid_plugin
        pimid_lib
        ${CMAKE_DL_LIBS}
    )
endif()

# libpimid_mpi — thread-based MPI library for unified PIM simulation
# LD_PRELOAD this into ZSim to run N MPI ranks as threads in one process.
# Magic ops inject NoC + hierarchy timing per MPI message.
add_library(pimid_mpi SHARED
    src/parallel_abstraction/pimid_mpi.cpp
)
target_include_directories(pimid_mpi PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(pimid_mpi PRIVATE
    Threads::Threads
    ${CMAKE_DL_LIBS}   # dlsym(RTLD_NEXT): 1.6 __libc_start_main interposer
)
target_compile_options(pimid_mpi PRIVATE
    -fvisibility=default   # MPI symbols must be visible for LD_PRELOAD interception
    -Wall -Wextra -O2
    -Wno-unused-parameter
)
set_target_properties(pimid_mpi PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# Main PIMID binary - unified entry point for all simulation modes
# Supports: standalone, host, device, and host/device co-simulation
add_executable(pimid src/main.cpp)

# 1.11.25: NVSim cache warmer. The pregenerated set is what runs actually read,
# so a cache schema change must be followed by re-characterization or the new
# fields never exist for the corpus. Drives NVSimWrapper for an explicit
# (type, capacity, node, width) tuple -- the cache key itself.
add_executable(nvsim_warm tools/nvsim_warm.cpp)
target_link_libraries(nvsim_warm pimid_lib)
target_link_libraries(pimid pimid_lib)
if(BUILD_PLUGINS)
    target_link_libraries(pimid pimid_plugin)
endif()
if(HDF5_FOUND)
    target_include_directories(pimid PRIVATE ${HDF5_INCLUDE_DIRS})
    target_link_libraries(pimid ${HDF5_LIBRARIES} ${HDF5_HL_LIBRARIES})
endif()
# Link zsim_trace static library for in-process trace replay
if(TARGET zsim_trace)
    target_link_libraries(pimid zsim_trace)
    target_include_directories(pimid PRIVATE ${PROJECT_SOURCE_DIR}/external/zsim/src)
endif()

# Install targets
install(TARGETS pimid_lib pimid pimid_mpi
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/ DESTINATION include/pimid)
install(DIRECTORY examples/ DESTINATION share/pimid/examples)

# Summary
message(STATUS "")
message(STATUS "PIMID Configuration Summary:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "  C++ flags: ${CMAKE_CXX_FLAGS}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  YAML-CPP found: ${yaml-cpp_FOUND}")
message(STATUS "  External models:")
if(HAVE_RAMULATOR)
    message(STATUS "    - Ramulator2: YES")
endif()
if(HAVE_CACTI7)
    message(STATUS "    - CACTI 7.0: YES")
endif()
if(HAVE_NVSIM)
    message(STATUS "    - NVSim: YES (namespaced)")
endif()
if(HAVE_MCPAT)
    message(STATUS "    - McPAT: YES (using CACTI 7)")
endif()
if(HAVE_GARNET)
    message(STATUS "    - Garnet: YES")
endif()
if(HDF5_FOUND)
    message(STATUS "    - HDF5: YES (real per-access trace conversion)")
else()
    message(STATUS "    - HDF5: NO (synthetic trace fallback)")
endif()
if(HAVE_QEMU_PLUGINS)
    message(STATUS "    - QEMU plugins: YES (libpimid_trace.so, libzsim_qemu.so)")
else()
    message(STATUS "    - QEMU plugins: NO")
endif()
message(STATUS "  MPI library: libpimid_mpi.so (unified thread-based MPI)")
message(STATUS "")

