cmake_minimum_required(VERSION 2.8)
PROJECT(SESC)

FIND_PACKAGE(FLEX)
FIND_PACKAGE(BISON)

# Handle options
OPTION(SMP "Bus-backed SMP Processor" ON)
OPTION(CMP "Booksim-backed NoC Processor" OFF)
OPTION(TM "Enable Hardware Transactional Memory" ON)
# Either SMP or CMP
IF(CMP)
    SET(SMP OFF)
ENDIF(CMP)

# Specify some global settings
ADD_COMPILE_OPTIONS(-Wall -pipe -Wno-unused)
ADD_COMPILE_OPTIONS(-fno-strict-aliasing -ffast-math)
ADD_DEFINITIONS(-DLINUX -DPOSIX_MEMALIGN -DMIPS_EMUL -DCHECK_STALL)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR})

# Set executable path to current directory instead of under libsmp/libcmp
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Specify option-specific #defines
IF(SMP)
    ADD_DEFINITIONS(-DSESC_SMP)
    IF(TM)
        ADD_DEFINITIONS(-DTM)
    ENDIF(TM)
    INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/libsuc)

    ADD_SUBDIRECTORY(libsmp)
ELSEIF(CMP)
    # XXX: Need to add both
    ADD_DEFINITIONS(-DSESC_SMP -DSESC_CMP)
    IF(TM)
        ADD_DEFINITIONS(-DTM)
    ENDIF(TM)
    INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/libsuc)

    ADD_SUBDIRECTORY(libbooksim)
    INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/libbooksim/header-deps.cmake)
    ADD_SUBDIRECTORY(libcmp)
ENDIF(SMP)

# Set subdirectories here to include above #defines
ADD_SUBDIRECTORY(libll)
ADD_SUBDIRECTORY(libsuc)
ADD_SUBDIRECTORY(libcore)
ADD_SUBDIRECTORY(libmem)
ADD_SUBDIRECTORY(libemul)
IF(TM)
    ADD_SUBDIRECTORY(libTM)
ENDIF(TM)
