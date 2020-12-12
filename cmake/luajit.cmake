
#
# LuaJIT configuration file.
#
# A copy of LuaJIT is maintained within Tarantool source. It's
# located in third_party/luajit.
#
# Instead of this copy, Tarantool can be compiled with LuaJIT at
# the given prefix. This is used when compiling Tarantool as a
# part of a distribution, e.g. Debian.
#
# To explicitly request use of the bundled LuaJIT, add
# -DENABLE_BUNDLED_LUAJIT=True to CMake configuration flags.
# To explicitly request use of LuaJIT at the given prefix, use
# -DLUAJIT_PREFIX=/path/to/LuaJIT.
#
# These two options are incompatible with each other.
#
# If neither of the two options is given, this script
# first attempts to use the system-installed LuaJIT
# and, in case it is not present or can not be used,
# falls back to the bundled one.
#
# LUAJIT_FOUND
# LUAJIT_LIBRARIES
# LUAJIT_INCLUDE_DIRS
#

# Bundled LuaJIT paths.
set(LUAJIT_BUNDLED_PREFIX "${PROJECT_BINARY_DIR}/third_party/luajit/src")

#
# LuaJIT options.
#
option(LUAJIT_PREFIX "Build with LuaJIT at the given path" "")
option(ENABLE_BUNDLED_LUAJIT "Build with bundled LuaJIT (default)" ON)

if(ENABLE_BUNDLED_LUAJIT AND LUAJIT_BUNDLED_PREFIX STREQUAL LUAJIT_PREFIX)
    message(FATAL_ERROR
        "Choose one: either LuaJIT in ${LUAJIT_PREFIX} or the bundled one.")
endif()

if(LUAJIT_PREFIX)
    # trying to build with specified LuaJIT.
    find_path(LUAJIT_INCLUDE "lua.h" ${LUAJIT_PREFIX} NO_DEFAULT_PATH)
    find_library(LUAJIT_LIB "luajit" ${LUAJIT_PREFIX} NO_DEFAULT_PATH)
    if(NOT LUAJIT_INCLUDE OR NOT LUAJIT_LIB)
        message (FATAL_ERROR "Couldn't find LuaJIT in '${LUAJIT_PREFIX}'")
    endif()
    # Check if there is a usable LuaJIT at the given prefix path.
    # See cmake/luatest.cpp for a description.
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/luatest.cpp" LUAJIT_TEST)
    set(CMAKE_REQUIRED_INCLUDES "${LUAJIT_INCLUDE}")
    set(CMAKE_REQUIRED_LIBRARIES "${LUAJIT_LIB}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(CMAKE_REQUIRED_LIBRARIES "-ldl ${CMAKE_REQUIRED_LIBRARIES}")
    endif()
    CHECK_CXX_SOURCE_RUNS("${LUAJIT_TEST}" LUAJIT_RUNS)
    if (LUAJIT_RUNS)
        message(STATUS "LuaJIT at ${LUAJIT_PREFIX} is suitable for use.")
    else()
        message(FATAL_ERROR "LuaJIT at ${LUAJIT_PREFIX} is NOT suitable for use.")
    endif()
    unset(LUAJIT_RUNS)
    unset(LUAJIT_TEST)
    unset(CMAKE_REQUIRED_LIBRARIES)
    unset(CMAKE_REQUIRED_INCLUDES)
else()
    if(TARGET_OS_DARWIN)
        # Necessary to make LuaJIT work on Darwin, see
        # http://luajit.org/install.html
        set(CMAKE_EXE_LINKER_FLAGS ${CMAKE_EXE_LINKER_FLAGS}
            "-pagezero_size 10000 -image_base 100000000")
    endif()
    set(LUAJIT_CMAKE_ARGS
	-DCMAKE_INSTALL_PREFIX=${MODULE_INCLUDEDIR}
	-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
	-DBUILDMODE=static
	-DLUAJIT_SMART_STRINGS=ON
	-DLUAJIT_ENABLE_GC64=${LUAJIT_ENABLE_GC64}
    )
    add_definitions(-DLUAJIT_SMART_STRINGS=1)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
	list(APPEND LUAJIT_CMAKE_ARGS
		-DLUAJIT_USE_APICHECK=ON
		-DLUAJIT_USE_ASSERT=ON)
    endif()
    if(ENABLE_VALGRIND)
	list(APPEND LUAJIT_CMAKE_ARGS
		-DLUAJIT_USE_SYSMALLOC=ON
		-DLUAJIT_USE_VALGRIND=ON)
    endif()
    # AddressSanitizer - CFLAGS were set globaly.
    if(ENABLE_ASAN)
        list(APPEND LUAJIT_CMAKE_ARGS -DLUAJIT_USE_ASAN=ON)
	# set(luajit_ldflags ${luajit_ldflags} -fsanitize=address)
    endif()
    
    set(LUAJIT_C_FLAGS ${CMAKE_C_FLAGS})
    set(LUAJIT_LDFLAGS ${CMAKE_EXE_LINKER_FLAGS})
    # separate_arguments(LUAJIT_C_FLAGS)
    # separate_arguments(LUAJIT_LDFLAGS)
    # Add all COMPILE_DEFINITIONS to xcflags.
    get_property(defs DIRECTORY PROPERTY COMPILE_DEFINITIONS)
    foreach(def ${defs})
        set(LUAJIT_C_FLAGS "${LUAJIT_C_FLAGS} -D${def}")
    endforeach()

    ExternalProject_Add(luajit
	PREFIX "${PROJECT_SOURCE_DIR}/third_party/luajit/"
	SOURCE_DIR "${PROJECT_SOURCE_DIR}/third_party/luajit/"
	BINARY_DIR "${PROJECT_SOURCE_DIR}/third_party/luajit/"
	BUILD_COMMAND ${CMAKE_MAKE_PROGRAM} libluajit
	CMAKE_ARGS ${LUAJIT_CMAKE_ARGS} -DCMAKE_C_FLAGS=${LUAJIT_C_FLAGS}
	TEST_EXCLUDE_FROM_MAIN ON
    )
    set(LUAJIT_PREFIX "${LUAJIT_BUNDLED_PREFIX}")
    set(LUAJIT_INCLUDE "${LUAJIT_PREFIX}")
    set(LUAJIT_LIB "${LUAJIT_PREFIX}/libluajit.a")
    add_custom_target(libluajit DEPENDS ${LUAJIT_LIB})
    add_dependencies(build_bundled_libs libluajit)
    include_directories("${PROJECT_BINARY_DIR}/third_party/luajit/src/")
    #
    #    set(luajit_cc ${CMAKE_C_COMPILER} ${CMAKE_C_COMPILER_ARG1})
    #    set(luajit_hostcc ${CMAKE_HOST_C_COMPILER})
    #    # CMake rules concerning strings and lists of strings are
    #    # weird.
    #    #   set (foo "1 2 3") defines a string, while
    #    #   set (foo 1 2 3) defines a list.
    #    # Use separate_arguments() to turn a string into a list
    #    # (splits at ws). It appears that variable expansion rules are
    #    # context-dependent.
    #    # With the current arrangement add_custom_command() does the
    #    # the right thing. We can even handle pathnames with spaces
    #    # though a path with an embeded semicolon or a quotation mark
    #    # will most certainly wreak havok.
    #    #
    #    # This stuff is extremely fragile, proceed with caution.
    #    if(CC_HAS_WNO_PARENTHESES_EQUALITY)
    #        set(luajit_cflags ${luajit_cflags} -Wno-parentheses-equality)
    #    endif()
    #    if(CC_HAS_WNO_TAUTOLOGICAL_COMPARE)
    #        set(luajit_cflags ${luajit_cflags} -Wno-tautological-compare)
    #    endif()
    #    if(CC_HAS_WNO_MISLEADING_INDENTATION)
    #        set(luajit_cflags ${luajit_cflags} -Wno-misleading-indentation)
    #    endif()
    #    if(CC_HAS_WNO_VARARGS)
    #        set(luajit_cflags ${luajit_cflags} -Wno-varargs)
    #    endif()
    #    if (CC_HAS_WNO_IMPLICIT_FALLTHROUGH)
    #        set(luajit_cflags ${luajit_cflags} -Wno-implicit-fallthrough)
    #    endif()
    #
    #    if(CMAKE_SYSTEM_NAME STREQUAL Darwin)
    #        # Pass sysroot - prepended in front of system header/lib dirs,
    #        # i.e. <sysroot>/usr/include, <sysroot>/usr/lib.
    #        # Needed for XCode users without command line tools installed,
    #        # they have headers/libs deep inside /Applications/Xcode.app/...
    #        if(NOT "${CMAKE_OSX_SYSROOT}" STREQUAL "")
    #            set(luajit_cflags ${luajit_cflags} ${CMAKE_C_SYSROOT_FLAG} ${CMAKE_OSX_SYSROOT})
    #            set(luajit_ldflags ${luajit_ldlags} ${CMAKE_C_SYSROOT_FLAG} ${CMAKE_OSX_SYSROOT})
    #            set(luajit_hostcc ${luajit_hostcc} ${CMAKE_C_SYSROOT_FLAG} ${CMAKE_OSX_SYSROOT})
    #        endif()
    #        # Pass deployment target
    #        if("${CMAKE_OSX_DEPLOYMENT_TARGET}" STREQUAL "")
    #            # Default to 10.6 since @rpath support is NOT available in
    #            # earlier versions, needed by AddressSanitizer.
    #            set(luajit_osx_deployment_target 10.6)
    #        else()
    #            set(luajit_osx_deployment_target ${CMAKE_OSX_DEPLOYMENT_TARGET})
    #        endif()
    #        set(luajit_ldflags
    #            ${luajit_ldflags} -Wl,-macosx_version_min,${luajit_osx_deployment_target})
    #    endif()
    #    if(ENABLE_GCOV)
    #        set(luajit_ccdebug ${luajit_ccdebug} -fprofile-arcs -ftest-coverage)
    #    endif()
    #    # Pass the same toolchain that is used for building of
    #    # tarantool itself, because tools from different toolchains
    #    # can be incompatible. A compiler and a linker are already set
    #    # above.
    #    set(luajit_ld ${CMAKE_LINKER})
    #    set(luajit_ar ${CMAKE_AR} rcus)
    #    # Enablibg LTO for luajit if DENABLE_LTO set.
    #    if(ENABLE_LTO)
    #        message(STATUS "Enable LTO for luajit")
    #        set(luajit_ldflags ${luajit_ldflags} ${LDFLAGS_LTO})
    #        message(STATUS "ld: " ${luajit_ldflags})
    #        set(luajit_cflags ${luajit_cflags} ${CFLAGS_LTO})
    #        message(STATUS "cflags: " ${luajit_cflags})
    #        set(luajit_ar  ${AR_LTO} rcus)
    #        message(STATUS "ar: " ${luajit_ar})
    #    endif()
    #    set(luajit_strip ${CMAKE_STRIP})
    #
    #    set(luajit_buildoptions
    #        BUILDMODE=static
    #        HOST_CC="${luajit_hostcc}"
    #        TARGET_CC="${luajit_cc}"
    #        TARGET_CFLAGS="${luajit_cflags}"
    #        TARGET_LD="${luajit_ld}"
    #        TARGET_LDFLAGS="${luajit_ldflags}"
    #        TARGET_AR="${luajit_ar}"
    #        TARGET_STRIP="${luajit_strip}"
    #        TARGET_SYS="${CMAKE_SYSTEM_NAME}"
    #        CCOPT="${luajit_ccopt}"
    #        CCDEBUG="${luajit_ccdebug}"
    #        XCFLAGS="${luajit_xcflags}"
    #        Q=''
    #        # We need to set MACOSX_DEPLOYMENT_TARGET to at least 10.6,
    #        # because 10.4 SDK (which is set by default in LuaJIT's
    #        # Makefile) is not longer included in Mac OS X Mojave 10.14.
    #        # See also https://github.com/LuaJIT/LuaJIT/issues/484
    #        MACOSX_DEPLOYMENT_TARGET="${luajit_osx_deployment_target}")
    #    if(PROJECT_BINARY_DIR STREQUAL PROJECT_SOURCE_DIR)
    #        add_custom_command(OUTPUT ${PROJECT_BINARY_DIR}/third_party/luajit/src/libluajit.a
    #            WORKING_DIRECTORY ${PROJECT_BINARY_DIR}/third_party/luajit
    #            COMMAND $(MAKE) ${luajit_buildoptions} clean
    #            COMMAND $(MAKE) -C src ${luajit_buildoptions} jit/vmdef.lua libluajit.a
    #            DEPENDS ${CMAKE_SOURCE_DIR}/CMakeCache.txt
    #        )
    #    else()
    #        add_custom_command(OUTPUT ${PROJECT_BINARY_DIR}/third_party/luajit
    #            COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_BINARY_DIR}/third_party/luajit"
    #        )
    #        add_custom_command(OUTPUT ${PROJECT_BINARY_DIR}/third_party/luajit/src/libluajit.a
    #            WORKING_DIRECTORY ${PROJECT_BINARY_DIR}/third_party/luajit
    #            COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/third_party/luajit ${PROJECT_BINARY_DIR}/third_party/luajit
    #            COMMAND $(MAKE) ${luajit_buildoptions} clean
    #            COMMAND $(MAKE) -C src ${luajit_buildoptions} jit/vmdef.lua libluajit.a
    #            DEPENDS ${PROJECT_BINARY_DIR}/CMakeCache.txt ${PROJECT_BINARY_DIR}/third_party/luajit
    #        )
    #    endif()
    #    unset(luajit_buildoptions)
    #    set(inc ${PROJECT_SOURCE_DIR}/third_party/luajit/src)
    #    install(FILES ${inc}/lua.h ${inc}/lualib.h ${inc}/lauxlib.h
    #        ${inc}/luaconf.h ${inc}/lua.hpp ${inc}/luajit.h ${inc}/lmisclib.h
    #        DESTINATION )
endif()

set(LuaJIT_FIND_REQUIRED TRUE)
find_package_handle_standard_args(LuaJIT
    REQUIRED_VARS LUAJIT_INCLUDE LUAJIT_LIB)
set(LUAJIT_INCLUDE_DIRS ${LUAJIT_INCLUDE})
set(LUAJIT_LIBRARIES ${LUAJIT_LIB})
