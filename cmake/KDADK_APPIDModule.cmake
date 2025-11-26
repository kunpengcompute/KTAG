# clean modules info which needs to be recalculated
set(KDADK_APPID_MODULES_PUBLIC         "" CACHE INTERNAL "List of KDADK_APPID modules marked for export")
set(KDADK_APPID_MODULES_BUILD          "" CACHE INTERNAL "List of KDADK_APPID modules included into the build")
set(KDADK_APPID_MODULES_DISABLED_USER  "" CACHE INTERNAL "List of KDADK_APPID modules explicitly disabled by user")
set(KDADK_APPID_MODULES_DISABLED_AUTO  "" CACHE INTERNAL "List of KDADK_APPID modules implicitly disabled due to dependencies")
set(KDADK_APPID_MODULES_DISABLED_FORCE "" CACHE INTERNAL "List of KDADK_APPID modules which can not be build in current configuration")

# adds dependencies to KDADK_APPID module
# Usage:
#   add_dependencies(KDADK_APPID_<name> [REQUIRED] [<list of dependencies>] [OPTIONAL <list of modules>])
# Notes:
# * <list of dependencies> - can include full names of modules or full pathes to shared/static libraries or cmake targets

macro(KDADK_APPID_add_dependencies full_modname)
    #we don't clean the dependencies here to allow this macro several times for every module
    foreach(d "REQUIRED" ${ARGN})
        if(d STREQUAL "REQUIRED")
            set(__depsvar KDADK_APPID_MODULE_${full_modname}_REQ_DEPS)
        elseif(d STREQUAL "OPTIONAL")
            set(__depsvar KDADK_APPID_MODULE_${full_modname}_OPT_DEPS)
        else()
            list(APPEND ${__depsvar} "KDADK_APPID_${d}")
        endif()
    endforeach()
    unset(__depsvar)

    KDADK_APPID_list_unique(KDADK_APPID_MODULE_${full_modname}_REQ_DEPS)
    KDADK_APPID_list_unique(KDADK_APPID_MODULE_${full_modname}_OPT_DEPS)

    set(KDADK_APPID_MODULE_${full_modname}_REQ_DEPS ${KDADK_APPID_MODULE_${full_modname}_REQ_DEPS} CACHE INTERNAL "Required dependencies of ${full_modname} module")
    set(KDADK_APPID_MODULE_${full_modname}_OPT_DEPS ${KDADK_APPID_MODULE_${full_modname}_OPT_DEPS} CACHE INTERNAL "Optional dependencies of ${full_modname} module")

endmacro()

# declare new KDADK_APPID module in current folder
# Usage:
#   KDADK_APPID_add_module(<name> [INTERNAL|BINDINGS] [REQUIRED] [<list of dependencies>] [OPTIONAL <list of optional dependencies>])
# Example:
#   KDADK_APPID_add_module(yaom INTERNAL KDADK_APPID_core KDADK_APPID_highgui KDADK_APPID_flann OPTIONAL KDADK_APPID_gpu)
macro(KDADK_APPID_add_module _name)
    string(TOLOWER "${_name}" name)
    set(the_module     ${name})
    set(the_module_res ${name})

    # the first pass - collect modules info, the second pass - create targets
    if(KDADK_APPID_INITIAL_PASS)
        #guard agains redefinition
        if(";${KDADK_APPID_MODULES_BUILD};${KDADK_APPID_MODULES_DISABLED_USER};" MATCHES ";${the_module};")
            message(FATAL_ERROR "Redefinition of the ${the_module} module.
            at:                    ${CMAKE_CURRENT_SOURCE_DIR}
            previously defined at: ${KDADK_APPID_MODULE_${the_module}_LOCATION}
            ")
        endif()

        if(NOT DEFINED the_description)
            set(the_description "The ${name} KDADK_APPID module")
        endif()

        if(NOT DEFINED BUILD_${the_module}_INIT)
            set(BUILD_${the_module}_INIT ON)
        endif()


        # create option to enable/disable this module
        option(BUILD_${the_module} "Include ${the_module} module into the KDADK_APPID build" ${BUILD_${the_module}_INIT})

        # remember the module details
        set(KDADK_APPID_MODULE_${the_module}_DESCRIPTION "${the_description}"          CACHE INTERNAL "Brief description of ${the_module} module")
        set(KDADK_APPID_MODULE_${the_module}_LOCATION    "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "Location of ${the_module} module sources")

        # parse list of dependencies
        if("${ARGV1}" STREQUAL "INTERNAL" OR "${ARGV1}" STREQUAL "BINDINGS")
            set(KDADK_APPID_MODULE_${the_module}_CLASS "${ARGV1}" CACHE INTERNAL "The cathegory of the module")
            set(__KDADK_APPID_argn__ ${ARGN})
            list(REMOVE_AT __KDADK_APPID_argn__ 0)
            KDADK_APPID_add_dependencies(${the_module} ${__KDADK_APPID_argn__})
            unset(__KDADK_APPID_argn__)
        else()
            set(KDADK_APPID_MODULE_${the_module}_CLASS "PUBLIC" CACHE INTERNAL "The cathegory of the module")
            KDADK_APPID_add_dependencies(${the_module} ${ARGN})
            if(BUILD_${the_module})
                set(KDADK_APPID_MODULES_PUBLIC ${KDADK_APPID_MODULES_PUBLIC} "${the_module}" CACHE INTERNAL "List of KDADK_APPID modules marked for export")
            endif()
        endif()

        if(BUILD_${the_module})
            set(KDADK_APPID_MODULES_BUILD ${KDADK_APPID_MODULES_BUILD} "${the_module}" CACHE INTERNAL "List of KDADK_APPID modules included into the build")
        else()
            set(KDADK_APPID_MODULES_DISABLED_USER ${KDADK_APPID_MODULES_DISABLED_USER} "${the_module}" CACHE INTERNAL "List of KDADK_APPID modules explicitly disabled by user")
        endif()

        return()
    else(KDADK_APPID_INITIAL_PASS) #
        if(NOT BUILD_${the_module})
            return() # extra protection from redefinition
        endif()
        project(${the_module})
    endif(KDADK_APPID_INITIAL_PASS)
endmacro()

# excludes module from current configuration
macro(KDADK_APPID_disable_module module)
    set(__modname ${module})
    if(NOT __modname MATCHES "^KDADK_APPID_")
        set(__modname KDADK_APPID_${module})
    endif()
    list(APPEND KDADK_APPID_MODULES_DISABLED_FORCE "${__modname}")
    set(HAVE_${__modname} OFF CACHE INTERNAL "Module ${__modname} can not be built in current configuration")
    set(KDADK_APPID_MODULE_${__modname}_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "Location of ${__modname} module sources")
    set(KDADK_APPID_MODULES_DISABLED_FORCE "${KDADK_APPID_MODULES_DISABLED_FORCE}" CACHE INTERNAL "List of KDADK_APPID modules which can not be build in current configuration")
    if(BUILD_${__modname})
        # touch variable controlling build of the module to suppress "unused variable" CMake warning
    endif()
    unset(__modname)
    return() # leave the current folder
endmacro()

macro(__KDADK_APPID_module_turn_off the_module)
  list(REMOVE_ITEM KDADK_APPID_MODULES_DISABLED_AUTO "${the_module}")
  list(APPEND      KDADK_APPID_MODULES_DISABLED_AUTO "${the_module}")
  list(REMOVE_ITEM KDADK_APPID_MODULES_BUILD "${the_module}")
  list(REMOVE_ITEM KDADK_APPID_MODULES_PUBLIC "${the_module}")
  set(HAVE_${the_module} OFF CACHE INTERNAL "Module ${the_module} can not be built in current configuration")
endmacro()

macro(__KDADK_APPID_flatten_module_required_dependencies the_module)
    set(__flattened_deps "")
    set(__resolved_deps "")
    set(__req_depends ${KDADK_APPID_MODULE_${the_module}_REQ_DEPS})

    while(__req_depends)
        KDADK_APPID_list_pop_front(__req_depends __dep)

        #depends on self
        if(__dep STREQUAL the_module)
            __KDADK_APPID_module_turn_off(${the_module}) 
            break()

            #depends on disabled module
        elseif(";${KDADK_APPID_MODULES_DISABLED_USER};${KDADK_APPID_MODULES_DISABLED_AUTO};" MATCHES ";${__dep};")

            __KDADK_APPID_module_turn_off(${the_module}) # depends on disabled module
            #list(APPEND __flattened_deps "${__dep}")
            break() #TODO

            #depends on build-enabled module
        elseif(";${KDADK_APPID_MODULES_BUILD};" MATCHES ";${__dep};")

            if(";${__resolved_deps};" MATCHES ";${__dep};")
                list(APPEND __flattened_deps "${__dep}") # all dependencies of this module are already resolved
            else()
                # put all required subdependencies before this dependency and mark it as resolved
                list(APPEND __resolved_deps "${__dep}")
                list(INSERT __req_depends 0 ${KDADK_APPID_MODULE_${__dep}_REQ_DEPS} ${__dep})
            endif()

        elseif(__dep MATCHES "^KDADK_APPID_")
            __KDADK_APPID_module_turn_off(${the_module}) # depends on missing module
            message(WARNING "Unknown \"${__dep}\" module is listened in the dependencies of \"${the_module}\" module")
            break()
        else()
            # skip non-modules
        endif()
    endwhile()

    if(__flattened_deps)
        list(REMOVE_DUPLICATES __flattened_deps)
        set(KDADK_APPID_MODULE_${the_module}_DEPS ${__flattened_deps})
    else()
        set(KDADK_APPID_MODULE_${the_module}_DEPS "")
    endif()

    KDADK_APPID_clear_vars(__resolved_deps __flattened_deps __req_depends __dep)
endmacro()

macro(__KDADK_APPID_flatten_module_optional_dependencies the_module)
  set(__flattened_deps "")
  set(__resolved_deps "")
  set(__opt_depends ${KDADK_APPID_MODULE_${the_module}_REQ_DEPS} ${KDADK_APPID_MODULE_${the_module}_OPT_DEPS})

  while(__opt_depends)
    KDADK_APPID_list_pop_front(__opt_depends __dep)
    if(__dep STREQUAL the_module)
        __KDADK_APPID_module_turn_off(${the_module}) 
      break()

    elseif(";${KDADK_APPID_MODULES_BUILD};" MATCHES ";${__dep};")
      if(";${__resolved_deps};" MATCHES ";${__dep};")
        list(APPEND __flattened_deps "${__dep}") # all dependencies of this module are already resolved
      else()
        # put all subdependencies before this dependency and mark it as resolved
        list(APPEND __resolved_deps "${__dep}")
        list(INSERT __opt_depends 0 ${KDADK_APPID_MODULE_${__dep}_REQ_DEPS} ${KDADK_APPID_MODULE_${__dep}_OPT_DEPS} ${__dep})
      endif()
    else()
      # skip non-modules or missing modules
    endif()
  endwhile()

  if(__flattened_deps)
    list(REMOVE_DUPLICATES __flattened_deps)
    set(KDADK_APPID_MODULE_${the_module}_DEPS ${__flattened_deps})
  else()
    set(KDADK_APPID_MODULE_${the_module}_DEPS "")
  endif()

  KDADK_APPID_clear_vars(__resolved_deps __flattened_deps __opt_depends __dep)
endmacro()

macro(__KDADK_APPID_flatten_module_dependencies)

    foreach(m ${KDADK_APPID_MODULES_DISABLED_USER})
        set(HAVE_${m} OFF CACHE INTERNAL "Module ${m} will not be built in current configuration")
    endforeach()

    foreach(m ${KDADK_APPID_MODULES_BUILD})
        set(HAVE_${m} ON CACHE INTERNAL "Module ${m} will be built in current configuration")
        __KDADK_APPID_flatten_module_required_dependencies(${m})
        set(KDADK_APPID_MODULE_${m}_DEPS ${KDADK_APPID_MODULE_${m}_DEPS} CACHE INTERNAL "Flattened required dependencies of ${m} module")
    endforeach()

    foreach(m ${KDADK_APPID_MODULES_BUILD})
        __KDADK_APPID_flatten_module_optional_dependencies(${m})

        # save dependencies from other modules
        set(KDADK_APPID_MODULE_${m}_DEPS ${KDADK_APPID_MODULE_${m}_DEPS} CACHE INTERNAL "Flattened dependencies of ${m} module")
        # save extra dependencies
        set(KDADK_APPID_MODULE_${m}_DEPS_EXT ${KDADK_APPID_MODULE_${m}_REQ_DEPS} ${KDADK_APPID_MODULE_${m}_OPT_DEPS})
        if(KDADK_APPID_MODULE_${m}_DEPS_EXT AND KDADK_APPID_MODULE_${m}_DEPS)
            list(REMOVE_ITEM KDADK_APPID_MODULE_${m}_DEPS_EXT ${KDADK_APPID_MODULE_${m}_DEPS})
        endif()

        set(KDADK_APPID_MODULE_${m}_DEPS_EXT ${KDADK_APPID_MODULE_${m}_DEPS_EXT} CACHE INTERNAL "Extra dependencies of ${m} module")
    endforeach()

    # order modules by dependencies
    set(KDADK_APPID_MODULES_BUILD_ "")
    foreach(m ${KDADK_APPID_MODULES_BUILD})
        list(APPEND KDADK_APPID_MODULES_BUILD_ ${KDADK_APPID_MODULE_${m}_DEPS} ${m})
    endforeach()
    KDADK_APPID_list_unique(KDADK_APPID_MODULES_BUILD_)

    set(KDADK_APPID_MODULES_PUBLIC        ${KDADK_APPID_MODULES_PUBLIC}        CACHE INTERNAL "List of KDADK_APPID modules marked for export")
    set(KDADK_APPID_MODULES_BUILD         ${KDADK_APPID_MODULES_BUILD_}        CACHE INTERNAL "List of KDADK_APPID modules included into the build")
    set(KDADK_APPID_MODULES_DISABLED_AUTO ${KDADK_APPID_MODULES_DISABLED_AUTO} CACHE INTERNAL "List of KDADK_APPID modules implicitly disabled due to dependencies")
endmacro()
# collect modules from specified directories
# NB: must be called only once!
macro(KDADK_APPID_glob_modules)

    if(DEFINED KDADK_APPID_INITIAL_PASS)
        message(FATAL_ERROR "Perf_IPP has already loaded its modules. Calling KDADK_APPID_glob_modules second time is not allowed.")
    endif()

    set(__directories_observed "")

    # collect modules
    set(KDADK_APPID_INITIAL_PASS ON)

    foreach(__path ${ARGN})
        KDADK_APPID_get_real_path(__path "${__path}")

        list(FIND __directories_observed "${__path}" __pathIdx)
        if(__pathIdx GREATER -1)
            message(FATAL_ERROR "The directory ${__path} is observed for KDADK_APPID modules second time.")
        endif()

        list(APPEND __directories_observed "${__path}")
        message("--Source code path = ${__path}")

        file(GLOB __KDADK_APPIDmodules RELATIVE "${__path}" "${__path}/*")

        if(__KDADK_APPIDmodules)
            list(SORT __KDADK_APPIDmodules)
            foreach(mod ${__KDADK_APPIDmodules})

                KDADK_APPID_get_real_path(__modpath "${__path}/${mod}")
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

    KDADK_APPID_clear_vars(__KDADK_APPIDmodules __directories_observed __path __modpath __pathIdx)

    # resolve dependencies 
     __KDADK_APPID_flatten_module_dependencies()

    # create modules
    set(KDADK_APPID_INITIAL_PASS OFF PARENT_SCOPE)
    set(KDADK_APPID_INITIAL_PASS OFF)

    foreach(m ${KDADK_APPID_MODULES_BUILD})
        if(m MATCHES "^KDADK_APPID_")
            string(REGEX REPLACE "^KDADK_APPID_" "" __shortname "${m}")
            add_subdirectory("${KDADK_APPID_MODULE_${m}_LOCATION}" "${CMAKE_CURRENT_BINARY_DIR}/${__shortname}")
        else()
            add_subdirectory("${KDADK_APPID_MODULE_${m}_LOCATION}" "${CMAKE_CURRENT_BINARY_DIR}/${m}")
        endif()
    endforeach()
    unset(__shortname)
endmacro()

# setup include paths for the list of passed modules
macro(KDADK_APPID_include_modules)
    foreach(d ${ARGN})
        if(d MATCHES "^KDADK_APPID_" AND HAVE_${d})
            if (EXISTS "${KDADK_APPID_MODULE_${d}_LOCATION}/include")
                KDADK_APPID_include_directories("${KDADK_APPID_MODULE_${d}_LOCATION}/include")
            endif()
        elseif(EXISTS "${d}")
            KDADK_APPID_include_directories("${d}")
        endif()
    endforeach()
endmacro()

# setup include path for KDADK_APPID headers for specified module
# KDADK_APPID_module_include_directories(<extra include directories/extra include modules>)
macro(KDADK_APPID_module_include_directories)
    KDADK_APPID_include_directories("${KDADK_APPID_MODULE_${the_module}_LOCATION}/include"
        "${KDADK_APPID_MODULE_${the_module}_LOCATION}/src"
        "${CMAKE_CURRENT_BINARY_DIR}" # for precompiled headers
        )
    KDADK_APPID_include_modules(${KDADK_APPID_MODULE_${the_module}_DEPS} ${ARGN})
endmacro()

# sets header and source files for the current module
# NB: all files specified as headers will be installed
# Usage:
# KDADK_APPID_set_module_sources([HEADERS] <list of files> [SOURCES] <list of files>)
macro(KDADK_APPID_set_module_sources)
    set(KDADK_APPID_MODULE_${the_module}_HEADERS "")
    set(KDADK_APPID_MODULE_${the_module}_SOURCES "")

    foreach(f "HEADERS" ${ARGN})
        if(f STREQUAL "HEADERS" OR f STREQUAL "SOURCES")
            set(__filesvar "KDADK_APPID_MODULE_${the_module}_${f}")
        else()
            list(APPEND ${__filesvar} "${f}")
        endif()
    endforeach()

    # the hacky way to embeed any files into the KDADK_APPID without modification of its build system. This part will be rewritten
    #if(COMMAND KDADK_APPID_get_module_external_sources)
    #  KDADK_APPID_get_module_external_sources()
    #endif()

    # use full paths for module to be independent from the module location
    KDADK_APPID_convert_to_full_paths(KDADK_APPID_MODULE_${the_module}_HEADERS)

    set(KDADK_APPID_MODULE_${the_module}_HEADERS ${KDADK_APPID_MODULE_${the_module}_HEADERS} CACHE INTERNAL "List of header files for ${the_module}")
    set(KDADK_APPID_MODULE_${the_module}_SOURCES ${KDADK_APPID_MODULE_${the_module}_SOURCES} CACHE INTERNAL "List of source files for ${the_module}")
endmacro()

# creates KDADK_APPID module in current folder
# creates new target, configures standard dependencies, compilers flags, install rules
# Usage:
#   KDADK_APPID_create_module(<extra link dependencies>)
#   KDADK_APPID_create_module(SKIP_LINK)
macro(KDADK_APPID_create_module)
    ADD_LIBRARY(${the_module} ${KDADK_APPID_MODULE_TYPE} ${KDADK_APPID_MODULE_${the_module}_HEADERS} ${KDADK_APPID_MODULE_${the_module}_SOURCES})

    if(NOT "${ARGN}" STREQUAL "SKIP_LINK")
        # 设置默认库依赖
        set(libraries ${KDADK_APPID_MODULE_${the_module}_DEPS} ${KDADK_APPID_MODULE_${the_module}_DEPS_EXT} ${KDADK_APPID_LINKER_LIBS} ${ARGN})
        # 链接库
        TARGET_LINK_LIBRARIES(${the_module} ${libraries})
    endif()

    if(ENABLE_SOLUTION_FOLDERS)
        SET_TARGET_PROPERTIES(${the_module} PROPERTIES FOLDER "modules")
    endif()

    SET_TARGET_PROPERTIES(${the_module} PROPERTIES
        OUTPUT_NAME "${the_module}"
        DEBUG_POSTFIX "${KDADK_APPID_DEBUG_POSTFIX}"
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
            VERSION   ${KDADK_APPID_LIBVERSION}
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

    #FILE(REMOVE_RECURSE ${KDADK_APPID_LIB_INSTALL_PATH})
    #FILE(REMOVE_RECURSE ${KDADK_APPID_INCLUDE_INSTALL_PATH})

#    INSTALL(TARGETS ${the_module}
#        RUNTIME DESTINATION ${KDADK_APPID_RUNTIME_INSTALL_PATH} 
#        LIBRARY DESTINATION ${KDADK_APPID_LIB_INSTALL_PATH}     
#        ARCHIVE DESTINATION ${KDADK_APPID_LIB_INSTALL_PATH}     
#        COMPONENT main
#            )

    # only "public" headers need to be installed
    if(KDADK_APPID_MODULE_${the_module}_HEADERS AND ";${KDADK_APPID_MODULES_PUBLIC};" MATCHES ";${the_module};")
        foreach(hdr ${KDADK_APPID_MODULE_${the_module}_HEADERS})
            if(hdr MATCHES "([^/])+.h(..)?$")
                INSTALL(FILES ${hdr} DESTINATION "${KDADK_APPID_INCLUDE_INSTALL_PATH}/${the_module_res}" COMPONENT main)
            endif()
        endforeach()
    endif()
endmacro()

# ensures that all passed modules are available
# sets KDADK_APPID_DEPENDENCIES_FOUND variable to TRUE/FALSE
macro(KDADK_APPID_check_dependencies)
    set(KDADK_APPID_DEPENDENCIES_FOUND TRUE)
    foreach(d ${ARGN})
        if(d MATCHES "^KDADK_APPID_[^ ]+$" AND NOT HAVE_${d})
            set(KDADK_APPID_DEPENDENCIES_FOUND FALSE)
            break()
        endif()
    endforeach()
endmacro()

# finds and sets headers and sources for the standard KDADK_APPID module
# Usage:
# KDADK_APPID_glob_module_sources(<extra sources&headers in the same format as used in KDADK_APPID_set_module_sources>)
macro(KDADK_APPID_glob_module_sources)
  file(GLOB_RECURSE lib_srcs "src/*.c" "src/*.cpp")
  file(GLOB_RECURSE lib_asm "src/*.s" "src/*.S")
  file(GLOB_RECURSE lib_int_hdrs "src/*.hpp" "src/*.h")
  file(GLOB lib_hdrs "include/${name}/*.hpp" "include/${name}/*.h")

  source_group("Src" FILES ${lib_srcs} ${lib_asm} ${lib_int_hdrs})
  source_group("Include" FILES ${lib_hdrs})

  KDADK_APPID_set_module_sources(${ARGN} HEADERS ${lib_hdrs} SOURCES ${lib_srcs} ${lib_asm} ${lib_int_hdrs})
endmacro()

# short command for adding simple KDADK_APPID module
# see KDADK_APPID_add_module for argument details
# Usage:
# KDADK_APPID_define_module(module_name  [INTERNAL] [REQUIRED] [<list of dependencies>] [OPTIONAL <list of optional dependencies>])
macro(KDADK_APPID_define_module module_name)
  KDADK_APPID_add_module(${module_name} ${ARGN})
  KDADK_APPID_glob_module_sources()
  KDADK_APPID_module_include_directories()
  KDADK_APPID_create_module()
endmacro()

macro(KDADK_APPID_glob_module_sources_neon)
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

  KDADK_APPID_set_module_sources(${ARGN} HEADERS ${lib_hdrs} SOURCES ${lib_srcs} ${lib_asm} ${lib_int_hdrs} ${core_src})
endmacro()

macro(KDADK_APPID_define_module_neon module_name)
  KDADK_APPID_add_module(${module_name} ${ARGN})
  KDADK_APPID_glob_module_sources_neon()
  KDADK_APPID_module_include_directories()
  KDADK_APPID_create_module()
endmacro()

macro(KDADK_APPID_glob_module_sources_time)
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

  KDADK_APPID_set_module_sources(${ARGN} HEADERS ${lib_hdrs} SOURCES ${lib_srcs} ${lib_asm} ${lib_int_hdrs} ${core_src})
endmacro()

macro(KDADK_APPID_define_module_time module_name)
  KDADK_APPID_add_module(${module_name} ${ARGN})
  KDADK_APPID_glob_module_sources_neon()
  KDADK_APPID_module_include_directories()
  KDADK_APPID_create_module()
endmacro()