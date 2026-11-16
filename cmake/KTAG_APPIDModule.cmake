# clean modules info which needs to be recalculated
set(KTAG_APPID_MODULES_PUBLIC         "" CACHE INTERNAL "List of KTAG_APPID modules marked for export")
set(KTAG_APPID_MODULES_BUILD          "" CACHE INTERNAL "List of KTAG_APPID modules included into the build")
set(KTAG_APPID_MODULES_DISABLED_USER  "" CACHE INTERNAL "List of KTAG_APPID modules explicitly disabled by user")
set(KTAG_APPID_MODULES_DISABLED_AUTO  "" CACHE INTERNAL "List of KTAG_APPID modules implicitly disabled due to dependencies")
set(KTAG_APPID_MODULES_DISABLED_FORCE "" CACHE INTERNAL "List of KTAG_APPID modules which can not be build in current configuration")

# adds dependencies to KTAG_APPID module
# Usage:
#   add_dependencies(KTAG_APPID_<name> [REQUIRED] [<list of dependencies>] [OPTIONAL <list of modules>])
# Notes:
# * <list of dependencies> - can include full names of modules or full pathes to shared/static libraries or cmake targets

macro(KTAG_APPID_add_dependencies full_modname)
    #we don't clean the dependencies here to allow this macro several times for every module
    foreach(d "REQUIRED" ${ARGN})
        if(d STREQUAL "REQUIRED")
            set(__depsvar KTAG_APPID_MODULE_${full_modname}_REQ_DEPS)
        elseif(d STREQUAL "OPTIONAL")
            set(__depsvar KTAG_APPID_MODULE_${full_modname}_OPT_DEPS)
        else()
            list(APPEND ${__depsvar} "KTAG_APPID_${d}")
        endif()
    endforeach()
    unset(__depsvar)

    KTAG_APPID_list_unique(KTAG_APPID_MODULE_${full_modname}_REQ_DEPS)
    KTAG_APPID_list_unique(KTAG_APPID_MODULE_${full_modname}_OPT_DEPS)

    set(KTAG_APPID_MODULE_${full_modname}_REQ_DEPS ${KTAG_APPID_MODULE_${full_modname}_REQ_DEPS} CACHE INTERNAL "Required dependencies of ${full_modname} module")
    set(KTAG_APPID_MODULE_${full_modname}_OPT_DEPS ${KTAG_APPID_MODULE_${full_modname}_OPT_DEPS} CACHE INTERNAL "Optional dependencies of ${full_modname} module")

endmacro()

# declare new KTAG_APPID module in current folder
# Usage:
#   KTAG_APPID_add_module(<name> [INTERNAL|BINDINGS] [REQUIRED] [<list of dependencies>] [OPTIONAL <list of optional dependencies>])
# Example:
#   KTAG_APPID_add_module(yaom INTERNAL KTAG_APPID_core KTAG_APPID_highgui KTAG_APPID_flann OPTIONAL KTAG_APPID_gpu)
macro(KTAG_APPID_add_module _name)
    string(TOLOWER "${_name}" name)
    set(the_module     ${name})
    set(the_module_res ${name})

    # the first pass - collect modules info, the second pass - create targets
    if(KTAG_APPID_INITIAL_PASS)
        #guard agains redefinition
        if(";${KTAG_APPID_MODULES_BUILD};${KTAG_APPID_MODULES_DISABLED_USER};" MATCHES ";${the_module};")
            message(FATAL_ERROR "Redefinition of the ${the_module} module.
            at:                    ${CMAKE_CURRENT_SOURCE_DIR}
            previously defined at: ${KTAG_APPID_MODULE_${the_module}_LOCATION}
            ")
        endif()

        if(NOT DEFINED the_description)
            set(the_description "The ${name} KTAG_APPID module")
        endif()

        if(NOT DEFINED BUILD_${the_module}_INIT)
            set(BUILD_${the_module}_INIT ON)
        endif()


        # create option to enable/disable this module
        option(BUILD_${the_module} "Include ${the_module} module into the KTAG_APPID build" ${BUILD_${the_module}_INIT})

        # remember the module details
        set(KTAG_APPID_MODULE_${the_module}_DESCRIPTION "${the_description}"          CACHE INTERNAL "Brief description of ${the_module} module")
        set(KTAG_APPID_MODULE_${the_module}_LOCATION    "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "Location of ${the_module} module sources")

        # parse list of dependencies
        if("${ARGV1}" STREQUAL "INTERNAL" OR "${ARGV1}" STREQUAL "BINDINGS")
            set(KTAG_APPID_MODULE_${the_module}_CLASS "${ARGV1}" CACHE INTERNAL "The cathegory of the module")
            set(__KTAG_APPID_argn__ ${ARGN})
            list(REMOVE_AT __KTAG_APPID_argn__ 0)
            KTAG_APPID_add_dependencies(${the_module} ${__KTAG_APPID_argn__})
            unset(__KTAG_APPID_argn__)
        else()
            set(KTAG_APPID_MODULE_${the_module}_CLASS "PUBLIC" CACHE INTERNAL "The cathegory of the module")
            KTAG_APPID_add_dependencies(${the_module} ${ARGN})
            if(BUILD_${the_module})
                set(KTAG_APPID_MODULES_PUBLIC ${KTAG_APPID_MODULES_PUBLIC} "${the_module}" CACHE INTERNAL "List of KTAG_APPID modules marked for export")
            endif()
        endif()

        if(BUILD_${the_module})
            set(KTAG_APPID_MODULES_BUILD ${KTAG_APPID_MODULES_BUILD} "${the_module}" CACHE INTERNAL "List of KTAG_APPID modules included into the build")
        else()
            set(KTAG_APPID_MODULES_DISABLED_USER ${KTAG_APPID_MODULES_DISABLED_USER} "${the_module}" CACHE INTERNAL "List of KTAG_APPID modules explicitly disabled by user")
        endif()

        return()
    else(KTAG_APPID_INITIAL_PASS) #
        if(NOT BUILD_${the_module})
            return() # extra protection from redefinition
        endif()
        project(${the_module})
    endif(KTAG_APPID_INITIAL_PASS)
endmacro()

# excludes module from current configuration
macro(KTAG_APPID_disable_module module)
    set(__modname ${module})
    if(NOT __modname MATCHES "^KTAG_APPID_")
        set(__modname KTAG_APPID_${module})
    endif()
    list(APPEND KTAG_APPID_MODULES_DISABLED_FORCE "${__modname}")
    set(HAVE_${__modname} OFF CACHE INTERNAL "Module ${__modname} can not be built in current configuration")
    set(KTAG_APPID_MODULE_${__modname}_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "Location of ${__modname} module sources")
    set(KTAG_APPID_MODULES_DISABLED_FORCE "${KTAG_APPID_MODULES_DISABLED_FORCE}" CACHE INTERNAL "List of KTAG_APPID modules which can not be build in current configuration")
    if(BUILD_${__modname})
        # touch variable controlling build of the module to suppress "unused variable" CMake warning
    endif()
    unset(__modname)
    return() # leave the current folder
endmacro()

macro(__KTAG_APPID_module_turn_off the_module)
  list(REMOVE_ITEM KTAG_APPID_MODULES_DISABLED_AUTO "${the_module}")
  list(APPEND      KTAG_APPID_MODULES_DISABLED_AUTO "${the_module}")
  list(REMOVE_ITEM KTAG_APPID_MODULES_BUILD "${the_module}")
  list(REMOVE_ITEM KTAG_APPID_MODULES_PUBLIC "${the_module}")
  set(HAVE_${the_module} OFF CACHE INTERNAL "Module ${the_module} can not be built in current configuration")
endmacro()

macro(__KTAG_APPID_flatten_module_required_dependencies the_module)
    set(__flattened_deps "")
    set(__resolved_deps "")
    set(__req_depends ${KTAG_APPID_MODULE_${the_module}_REQ_DEPS})

    while(__req_depends)
        KTAG_APPID_list_pop_front(__req_depends __dep)

        #depends on self
        if(__dep STREQUAL the_module)
            __KTAG_APPID_module_turn_off(${the_module})
            break()

            #depends on disabled module
        elseif(";${KTAG_APPID_MODULES_DISABLED_USER};${KTAG_APPID_MODULES_DISABLED_AUTO};" MATCHES ";${__dep};")

            __KTAG_APPID_module_turn_off(${the_module}) # depends on disabled module
            #list(APPEND __flattened_deps "${__dep}")
            break() #TODO

            #depends on build-enabled module
        elseif(";${KTAG_APPID_MODULES_BUILD};" MATCHES ";${__dep};")

            if(";${__resolved_deps};" MATCHES ";${__dep};")
                list(APPEND __flattened_deps "${__dep}") # all dependencies of this module are already resolved
            else()
                # put all required subdependencies before this dependency and mark it as resolved
                list(APPEND __resolved_deps "${__dep}")
                list(INSERT __req_depends 0 ${KTAG_APPID_MODULE_${__dep}_REQ_DEPS} ${__dep})
            endif()

        elseif(__dep MATCHES "^KTAG_APPID_")
            __KTAG_APPID_module_turn_off(${the_module}) # depends on missing module
            message(WARNING "Unknown \"${__dep}\" module is listened in the dependencies of \"${the_module}\" module")
            break()
        else()
            # skip non-modules
        endif()
    endwhile()

    if(__flattened_deps)
        list(REMOVE_DUPLICATES __flattened_deps)
        set(KTAG_APPID_MODULE_${the_module}_DEPS ${__flattened_deps})
    else()
        set(KTAG_APPID_MODULE_${the_module}_DEPS "")
    endif()

    KTAG_APPID_clear_vars(__resolved_deps __flattened_deps __req_depends __dep)
endmacro()

macro(__KTAG_APPID_flatten_module_optional_dependencies the_module)
  set(__flattened_deps "")
  set(__resolved_deps "")
  set(__opt_depends ${KTAG_APPID_MODULE_${the_module}_REQ_DEPS} ${KTAG_APPID_MODULE_${the_module}_OPT_DEPS})

  while(__opt_depends)
    KTAG_APPID_list_pop_front(__opt_depends __dep)
    if(__dep STREQUAL the_module)
        __KTAG_APPID_module_turn_off(${the_module})
      break()

    elseif(";${KTAG_APPID_MODULES_BUILD};" MATCHES ";${__dep};")
      if(";${__resolved_deps};" MATCHES ";${__dep};")
        list(APPEND __flattened_deps "${__dep}") # all dependencies of this module are already resolved
      else()
        # put all subdependencies before this dependency and mark it as resolved
        list(APPEND __resolved_deps "${__dep}")
        list(INSERT __opt_depends 0 ${KTAG_APPID_MODULE_${__dep}_REQ_DEPS} ${KTAG_APPID_MODULE_${__dep}_OPT_DEPS} ${__dep})
      endif()
    else()
      # skip non-modules or missing modules
    endif()
  endwhile()

  if(__flattened_deps)
    list(REMOVE_DUPLICATES __flattened_deps)
    set(KTAG_APPID_MODULE_${the_module}_DEPS ${__flattened_deps})
  else()
    set(KTAG_APPID_MODULE_${the_module}_DEPS "")
  endif()

  KTAG_APPID_clear_vars(__resolved_deps __flattened_deps __opt_depends __dep)
endmacro()

macro(__KTAG_APPID_flatten_module_dependencies)

    foreach(m ${KTAG_APPID_MODULES_DISABLED_USER})
        set(HAVE_${m} OFF CACHE INTERNAL "Module ${m} will not be built in current configuration")
    endforeach()

    foreach(m ${KTAG_APPID_MODULES_BUILD})
        set(HAVE_${m} ON CACHE INTERNAL "Module ${m} will be built in current configuration")
        __KTAG_APPID_flatten_module_required_dependencies(${m})
        set(KTAG_APPID_MODULE_${m}_DEPS ${KTAG_APPID_MODULE_${m}_DEPS} CACHE INTERNAL "Flattened required dependencies of ${m} module")
    endforeach()

    foreach(m ${KTAG_APPID_MODULES_BUILD})
        __KTAG_APPID_flatten_module_optional_dependencies(${m})

        # save dependencies from other modules
        set(KTAG_APPID_MODULE_${m}_DEPS ${KTAG_APPID_MODULE_${m}_DEPS} CACHE INTERNAL "Flattened dependencies of ${m} module")
        # save extra dependencies
        set(KTAG_APPID_MODULE_${m}_DEPS_EXT ${KTAG_APPID_MODULE_${m}_REQ_DEPS} ${KTAG_APPID_MODULE_${m}_OPT_DEPS})
        if(KTAG_APPID_MODULE_${m}_DEPS_EXT AND KTAG_APPID_MODULE_${m}_DEPS)
            list(REMOVE_ITEM KTAG_APPID_MODULE_${m}_DEPS_EXT ${KTAG_APPID_MODULE_${m}_DEPS})
        endif()

        set(KTAG_APPID_MODULE_${m}_DEPS_EXT ${KTAG_APPID_MODULE_${m}_DEPS_EXT} CACHE INTERNAL "Extra dependencies of ${m} module")
    endforeach()

    # order modules by dependencies
    set(KTAG_APPID_MODULES_BUILD_ "")
    foreach(m ${KTAG_APPID_MODULES_BUILD})
        list(APPEND KTAG_APPID_MODULES_BUILD_ ${KTAG_APPID_MODULE_${m}_DEPS} ${m})
    endforeach()
    KTAG_APPID_list_unique(KTAG_APPID_MODULES_BUILD_)

    set(KTAG_APPID_MODULES_PUBLIC        ${KTAG_APPID_MODULES_PUBLIC}        CACHE INTERNAL "List of KTAG_APPID modules marked for export")
    set(KTAG_APPID_MODULES_BUILD         ${KTAG_APPID_MODULES_BUILD_}        CACHE INTERNAL "List of KTAG_APPID modules included into the build")
    set(KTAG_APPID_MODULES_DISABLED_AUTO ${KTAG_APPID_MODULES_DISABLED_AUTO} CACHE INTERNAL "List of KTAG_APPID modules implicitly disabled due to dependencies")
endmacro()
# collect modules from specified directories
# NB: must be called only once!
macro(KTAG_APPID_glob_modules)

    if(DEFINED KTAG_APPID_INITIAL_PASS)
        message(FATAL_ERROR "Perf_IPP has already loaded its modules. Calling KTAG_APPID_glob_modules second time is not allowed.")
    endif()

    set(__directories_observed "")

    # collect modules
    set(KTAG_APPID_INITIAL_PASS ON)

    foreach(__path ${ARGN})
        KTAG_APPID_get_real_path(__path "${__path}")

        list(FIND __directories_observed "${__path}" __pathIdx)
        if(__pathIdx GREATER -1)
            message(FATAL_ERROR "The directory ${__path} is observed for KTAG_APPID modules second time.")
        endif()

        list(APPEND __directories_observed "${__path}")
        message("--Source code path = ${__path}")

        file(GLOB __KTAG_APPIDmodules RELATIVE "${__path}" "${__path}/*")

        if(__KTAG_APPIDmodules)
            list(SORT __KTAG_APPIDmodules)
            foreach(mod ${__KTAG_APPIDmodules})

                KTAG_APPID_get_real_path(__modpath "${__path}/${mod}")
                if(EXISTS "${__modpath}/CMakeLists.txt")

                    list(FIND __directories_observed "${__modpath}" __pathIdx)
                    if(__pathIdx GREATER -1)
                        message(FATAL_ERROR "The module from ${__modpath} is already loaded.")
                    endif()
                    list(APPEND __directories_observed "${__modpath}")

                    add_subdirectory("${__modpath}" "${CMAKE_CURRENT_BINARY_DIR}/${mod}/.${mod}")

                endif()
            endforeach()
        endif()
    endforeach()

    KTAG_APPID_clear_vars(__KTAG_APPIDmodules __directories_observed __path __modpath __pathIdx)

    # resolve dependencies
     __KTAG_APPID_flatten_module_dependencies()

    # create modules
    set(KTAG_APPID_INITIAL_PASS OFF PARENT_SCOPE)
    set(KTAG_APPID_INITIAL_PASS OFF)

    foreach(m ${KTAG_APPID_MODULES_BUILD})
        if(m MATCHES "^KTAG_APPID_")
            string(REGEX REPLACE "^KTAG_APPID_" "" __shortname "${m}")
            add_subdirectory("${KTAG_APPID_MODULE_${m}_LOCATION}" "${CMAKE_CURRENT_BINARY_DIR}/${__shortname}")
        else()
            add_subdirectory("${KTAG_APPID_MODULE_${m}_LOCATION}" "${CMAKE_CURRENT_BINARY_DIR}/${m}")
        endif()
    endforeach()
    unset(__shortname)
endmacro()

# setup include paths for the list of passed modules
macro(KTAG_APPID_include_modules)
    foreach(d ${ARGN})
        if(d MATCHES "^KTAG_APPID_" AND HAVE_${d})
            if (EXISTS "${KTAG_APPID_MODULE_${d}_LOCATION}/include")
                KTAG_APPID_include_directories("${KTAG_APPID_MODULE_${d}_LOCATION}/include")
            endif()
        elseif(EXISTS "${d}")
            KTAG_APPID_include_directories("${d}")
        endif()
    endforeach()
endmacro()

# setup include path for KTAG_APPID headers for specified module
# KTAG_APPID_module_include_directories(<extra include directories/extra include modules>)
macro(KTAG_APPID_module_include_directories)
    KTAG_APPID_include_directories("${KTAG_APPID_MODULE_${the_module}_LOCATION}/include"
        "${KTAG_APPID_MODULE_${the_module}_LOCATION}/src"
        "${CMAKE_CURRENT_BINARY_DIR}" # for precompiled headers
        )
    KTAG_APPID_include_modules(${KTAG_APPID_MODULE_${the_module}_DEPS} ${ARGN})
endmacro()

# sets header and source files for the current module
# NB: all files specified as headers will be installed
# Usage:
# KTAG_APPID_set_module_sources([HEADERS] <list of files> [SOURCES] <list of files>)
macro(KTAG_APPID_set_module_sources)
    set(KTAG_APPID_MODULE_${the_module}_HEADERS "")
    set(KTAG_APPID_MODULE_${the_module}_SOURCES "")

    foreach(f "HEADERS" ${ARGN})
        if(f STREQUAL "HEADERS" OR f STREQUAL "SOURCES")
            set(__filesvar "KTAG_APPID_MODULE_${the_module}_${f}")
        else()
            list(APPEND ${__filesvar} "${f}")
        endif()
    endforeach()

    # the hacky way to embeed any files into the KTAG_APPID without modification of its build system. This part will be rewritten
    #if(COMMAND KTAG_APPID_get_module_external_sources)
    #  KTAG_APPID_get_module_external_sources()
    #endif()

    # use full paths for module to be independent from the module location
    KTAG_APPID_convert_to_full_paths(KTAG_APPID_MODULE_${the_module}_HEADERS)

    set(KTAG_APPID_MODULE_${the_module}_HEADERS ${KTAG_APPID_MODULE_${the_module}_HEADERS} CACHE INTERNAL "List of header files for ${the_module}")
    set(KTAG_APPID_MODULE_${the_module}_SOURCES ${KTAG_APPID_MODULE_${the_module}_SOURCES} CACHE INTERNAL "List of source files for ${the_module}")
endmacro()

# creates KTAG_APPID module in current folder
# creates new target, configures standard dependencies, compilers flags, install rules
# Usage:
#   KTAG_APPID_create_module(<extra link dependencies>)
#   KTAG_APPID_create_module(SKIP_LINK)
macro(KTAG_APPID_create_module)
    ADD_LIBRARY(${the_module} ${KTAG_APPID_MODULE_TYPE} ${KTAG_APPID_MODULE_${the_module}_HEADERS} ${KTAG_APPID_MODULE_${the_module}_SOURCES})

    if(NOT "${ARGN}" STREQUAL "SKIP_LINK")
        # 设置默认库依赖
        set(libraries ${KTAG_APPID_MODULE_${the_module}_DEPS} ${KTAG_APPID_MODULE_${the_module}_DEPS_EXT} ${KTAG_APPID_LINKER_LIBS} ${ARGN})
        # 链接库
        TARGET_LINK_LIBRARIES(${the_module} ${libraries})
    endif()

    if(ENABLE_SOLUTION_FOLDERS)
        SET_TARGET_PROPERTIES(${the_module} PROPERTIES FOLDER "modules")
    endif()

    SET_TARGET_PROPERTIES(${the_module} PROPERTIES
        OUTPUT_NAME "${the_module}"
        DEBUG_POSTFIX "${KTAG_APPID_DEBUG_POSTFIX}"
        ARCHIVE_OUTPUT_DIRECTORY "${LIBRARY_OUTPUT_PATH}"
        LIBRARY_OUTPUT_DIRECTORY "${LIBRARY_OUTPUT_PATH}"
        RUNTIME_OUTPUT_DIRECTORY "${EXECUTABLE_OUTPUT_PATH}"
        COMPILE_FLAGS ${CMAKE_C_FLAGS}
        INSTALL_NAME_DIR lib
        )

    # For dynamic link numbering convenions
    if(NOT ANDROID)
        # Android SDK build scripts can include only .so files into final .apk
        # As result we should not set version properties for Android
        SET_TARGET_PROPERTIES(${the_module} PROPERTIES
            VERSION   ${KTAG_APPID_LIBVERSION}
            )
    endif()

    if(BUILD_SHARED_LIBS)
        if(MSVC)
            SET_TARGET_PROPERTIES(${the_module} PROPERTIES DEFINE_SYMBOL CVAPI_EXPORTS)
        else()
            ADD_DEFINITIONS(-DCVAPI_EXPORTS)
        endif()
    endif()

    if(MSVC)
        if(CMAKE_CROSSCOMPILING)
            SET_TARGET_PROPERTIES(${the_module} PROPERTIES LINK_FLAGS "/NODEFAULTLIB:secchk")
        endif()
        SET_TARGET_PROPERTIES(${the_module} PROPERTIES LINK_FLAGS "/NODEFAULTLIB:libc /DEBUG")
    endif()

    #FILE(REMOVE_RECURSE ${KTAG_APPID_LIB_INSTALL_PATH})
    #FILE(REMOVE_RECURSE ${KTAG_APPID_INCLUDE_INSTALL_PATH})

#    INSTALL(TARGETS ${the_module}
#        RUNTIME DESTINATION ${KTAG_APPID_RUNTIME_INSTALL_PATH}
#        LIBRARY DESTINATION ${KTAG_APPID_LIB_INSTALL_PATH}
#        ARCHIVE DESTINATION ${KTAG_APPID_LIB_INSTALL_PATH}
#        COMPONENT main
#            )

    # only "public" headers need to be installed
    if(KTAG_APPID_MODULE_${the_module}_HEADERS AND ";${KTAG_APPID_MODULES_PUBLIC};" MATCHES ";${the_module};")
        foreach(hdr ${KTAG_APPID_MODULE_${the_module}_HEADERS})
            if(hdr MATCHES "([^/])+.h(..)?$")
                INSTALL(FILES ${hdr} DESTINATION "${KTAG_APPID_INCLUDE_INSTALL_PATH}/${the_module_res}" COMPONENT main)
            endif()
        endforeach()
    endif()
endmacro()

# ensures that all passed modules are available
# sets KTAG_APPID_DEPENDENCIES_FOUND variable to TRUE/FALSE
macro(KTAG_APPID_check_dependencies)
    set(KTAG_APPID_DEPENDENCIES_FOUND TRUE)
    foreach(d ${ARGN})
        if(d MATCHES "^KTAG_APPID_[^ ]+$" AND NOT HAVE_${d})
            set(KTAG_APPID_DEPENDENCIES_FOUND FALSE)
            break()
        endif()
    endforeach()
endmacro()

# finds and sets headers and sources for the standard KTAG_APPID module
# Usage:
# KTAG_APPID_glob_module_sources(<extra sources&headers in the same format as used in KTAG_APPID_set_module_sources>)
macro(KTAG_APPID_glob_module_sources)
  file(GLOB_RECURSE lib_srcs "src/*.c" "src/*.cpp")
  file(GLOB_RECURSE lib_asm "src/*.s" "src/*.S")
  file(GLOB_RECURSE lib_int_hdrs "src/*.hpp" "src/*.h")
  file(GLOB lib_hdrs "include/${name}/*.hpp" "include/${name}/*.h")

  source_group("Src" FILES ${lib_srcs} ${lib_asm} ${lib_int_hdrs})
  source_group("Include" FILES ${lib_hdrs})

  KTAG_APPID_set_module_sources(${ARGN} HEADERS ${lib_hdrs} SOURCES ${lib_srcs} ${lib_asm} ${lib_int_hdrs})
endmacro()

# short command for adding simple KTAG_APPID module
# see KTAG_APPID_add_module for argument details
# Usage:
# KTAG_APPID_define_module(module_name  [INTERNAL] [REQUIRED] [<list of dependencies>] [OPTIONAL <list of optional dependencies>])
macro(KTAG_APPID_define_module module_name)
  KTAG_APPID_add_module(${module_name} ${ARGN})
  KTAG_APPID_glob_module_sources()
  KTAG_APPID_module_include_directories()
  KTAG_APPID_create_module()
endmacro()

macro(KTAG_APPID_glob_module_sources_neon)
  set(CMAKE_ASM_COMPILER ${CMAKE_C_COMPILER})
  set(CMAKE_ASM_CREATE_SHARED_LIBRARY
    "${CMAKE_ASM_COMPILER} -shared -o <TARGET> <OBJECTS>"
    )
  file(GLOB_RECURSE lib_srcs "src/*.c" "src/*.cpp")
  file(GLOB_RECURSE lib_asm "src/*.s" "src/*.S")
  file(GLOB_RECURSE lib_int_hdrs "src/*.hpp" "src/*.h")
  set(CORE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../core_sve/src")
  file(GLOB core_src
    "${CORE_SRC_DIR}/cpufeature.c"
    "${CORE_SRC_DIR}/version.c"
    )
  file(GLOB lib_hdrs "include/${name}/*.hpp" "include/${name}/*.h")

  source_group("Src" FILES ${lib_srcs} ${lib_asm} ${lib_int_hdrs} ${core_src})
  source_group("Include" FILES ${lib_hdrs})

  KTAG_APPID_set_module_sources(${ARGN} HEADERS ${lib_hdrs} SOURCES ${lib_srcs} ${lib_asm} ${lib_int_hdrs} ${core_src})
endmacro()

macro(KTAG_APPID_define_module_neon module_name)
  KTAG_APPID_add_module(${module_name} ${ARGN})
  KTAG_APPID_glob_module_sources_neon()
  KTAG_APPID_module_include_directories()
  KTAG_APPID_create_module()
endmacro()

macro(KTAG_APPID_glob_module_sources_time)
  file(GLOB_RECURSE lib_srcs "src/*.c" "src/*.cpp")
  file(GLOB_RECURSE lib_asm "src/*.s" "src/*.S")
  file(GLOB_RECURSE lib_int_hdrs "src/*.hpp" "src/*.h")
  set(CORE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../core_sve/src")
  file(GLOB core_src
    "${CORE_SRC_DIR}/cpufeature.c"
    "${CORE_SRC_DIR}/version.c"
    )
  file(GLOB lib_hdrs "include/${name}/*.hpp" "include/${name}/*.h")

  source_group("Src" FILES ${lib_srcs} ${lib_asm} ${lib_int_hdrs} ${core_src})
  source_group("Include" FILES ${lib_hdrs})

  KTAG_APPID_set_module_sources(${ARGN} HEADERS ${lib_hdrs} SOURCES ${lib_srcs} ${lib_asm} ${lib_int_hdrs} ${core_src})
endmacro()

macro(KTAG_APPID_define_module_time module_name)
  KTAG_APPID_add_module(${module_name} ${ARGN})
  KTAG_APPID_glob_module_sources_neon()
  KTAG_APPID_module_include_directories()
  KTAG_APPID_create_module()
endmacro()
