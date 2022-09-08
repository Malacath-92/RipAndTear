function(get_git_version OUTPUT_VAR)
    find_package(Git)

    if(NOT GIT_FOUND)
        SET(GIT_EXECUTABLE git)
    endif()

    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty=-modified
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE ${OUTPUT_VAR}
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ECHO_ERROR_VARIABLE
        COMMAND_ERROR_IS_FATAL ANY)
endfunction()
