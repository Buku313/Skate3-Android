function(skate3_compute_version out_var)
    set(one_value FLOOR_VERSION GIT_DESCRIBE_LONG GIT_DESCRIBE_EXACT)
    cmake_parse_arguments(ARG "" "${one_value}" "" ${ARGN})

    if(NOT "${ARG_GIT_DESCRIBE_EXACT}" STREQUAL "")
        if(ARG_GIT_DESCRIBE_EXACT MATCHES "^v([0-9]+\\.[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?(-[0-9A-Za-z][0-9A-Za-z.-]*)?)$")
            set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
            return()
        endif()
        message(FATAL_ERROR "skate3_compute_version: unparseable exact tag '${ARG_GIT_DESCRIBE_EXACT}'")
    endif()

    if("${ARG_GIT_DESCRIBE_LONG}" STREQUAL "")
        message(WARNING
            "skate3_compute_version: no v* tag reachable from HEAD. "
            "Falling back to ${ARG_FLOOR_VERSION}-dev.unknown. "
            "For CI release builds, fetch tags and build from a release tag.")
        set(${out_var} "${ARG_FLOOR_VERSION}-dev.unknown" PARENT_SCOPE)
        return()
    endif()

    if(NOT ARG_GIT_DESCRIBE_LONG MATCHES "^v[0-9]+\\.[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?(-[0-9A-Za-z][0-9A-Za-z.-]*)?-([0-9]+)-g([0-9a-f]+)$")
        message(FATAL_ERROR "skate3_compute_version: unparseable describe output '${ARG_GIT_DESCRIBE_LONG}'")
    endif()

    set(commit_count ${CMAKE_MATCH_4})
    set(short_sha ${CMAKE_MATCH_5})
    set(${out_var} "${ARG_FLOOR_VERSION}.${commit_count}-dev.g${short_sha}" PARENT_SCOPE)
endfunction()

function(skate3_resolve_version out_var)
    set(one_value FLOOR_VERSION SOURCE_DIR)
    cmake_parse_arguments(ARG "" "${one_value}" "" ${ARGN})

    if(NOT ARG_SOURCE_DIR)
        set(ARG_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
    endif()

    find_program(GIT_EXECUTABLE git)
    if(NOT GIT_EXECUTABLE)
        skate3_compute_version(result
            FLOOR_VERSION ${ARG_FLOOR_VERSION}
            GIT_DESCRIBE_LONG ""
            GIT_DESCRIBE_EXACT "")
        set(${out_var} "${result}" PARENT_SCOPE)
        return()
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match --match "v[0-9]*.[0-9]*"
        WORKING_DIRECTORY "${ARG_SOURCE_DIR}"
        OUTPUT_VARIABLE describe_exact
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE describe_exact_rc)
    if(NOT describe_exact_rc EQUAL 0)
        set(describe_exact "")
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --long --match "v[0-9]*.[0-9]*"
        WORKING_DIRECTORY "${ARG_SOURCE_DIR}"
        OUTPUT_VARIABLE describe_long
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE describe_long_rc)
    if(NOT describe_long_rc EQUAL 0)
        set(describe_long "")
    endif()

    skate3_compute_version(result
        FLOOR_VERSION ${ARG_FLOOR_VERSION}
        GIT_DESCRIBE_LONG "${describe_long}"
        GIT_DESCRIBE_EXACT "${describe_exact}")
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()
