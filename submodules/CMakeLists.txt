# --------------------------------------------------
# Add subdirectories directly into a folder in the IDE
# Inspired by https://stackoverflow.com/questions/45092198/cmake-how-do-i-change-properties-on-subdirectory-project-targets#
function(get_all_targets _result _dir)
    get_property(_subdirs DIRECTORY "${_dir}" PROPERTY SUBDIRECTORIES)

    foreach(_subdir IN LISTS _subdirs)
        get_all_targets(${_result} "${_subdir}")
    endforeach()

    get_property(_sub_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
    set(${_result} ${${_result}} ${_sub_targets} PARENT_SCOPE)
endfunction()

function(add_subdirectory_with_folder _folder_name _folder)
    add_subdirectory(${_folder} ${ARGN} EXCLUDE_FROM_ALL)
    get_all_targets(_targets "${_folder}")

    foreach(_target IN LISTS _targets)
        get_target_property(_target_type ${_target} TYPE)

        if(NOT _target_type STREQUAL "INTERFACE_LIBRARY")
            get_property(_current_folder_name TARGET ${_target} PROPERTY FOLDER)
            set_target_properties(${_target} PROPERTIES
                FOLDER "${_folder_name}/${_current_folder_name}")

            if(NOT _target_type STREQUAL "UTILITY")
                target_compile_options(${_target} PRIVATE
                    "/W0")
            endif()
        endif()
    endforeach()
endfunction()
