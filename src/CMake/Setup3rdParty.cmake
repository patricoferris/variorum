# Copyright 2019-2022 Lawrence Livermore National Security, LLC and other
# Variorum Project Developers. See the top-level LICENSE file for details.
#
# SPDX-License-Identifier: MIT

include(CMake/thirdparty/SetupHwloc.cmake)
include(CMake/thirdparty/SetupJansson.cmake)
if(VARIORUM_WITH_AMD)
include(CMake/thirdparty/Setupesmi.cmake)
endif()
if(VARIORUM_WITH_NVIDIA)
    include(CMake/thirdparty/FindNVML.cmake)
endif()

if(ENABLE_MPI)
    include(CMake/thirdparty/SetupRankstr.cmake)
endif()

if(BUILD_DOCS)
    find_package(Doxygen)
    include(CMake/thirdparty/FindSphinx.cmake)
endif()

if(BUILD_TESTS)
    add_definitions(-DGTEST_HAS_TR1_TUPLE=0)
    ################################
    # Enable GTest
    ################################

    #
    # We always want to build gtest as a static lib, however
    # it shares our "BUILD_SHARED_LIBS" option, so we need
    # to force this value to OFF, and then restore the
    # previous setting.
    #
    set(BSL_ORIG_VALUE ${BUILD_SHARED_LIBS})

    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory(thirdparty_builtin/googletest)

    set(BUILD_SHARED_LIBS ${BSL_ORIG_VALUE})

    enable_testing()
    include_directories(${gtest_SOURCE_DIR}/include ${gtest_SOURCE_DIR})
endif()
