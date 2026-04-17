function(add_git_ref target)
    if(NOT DEFINED GIT_REV)
        set(GIT_REV "HEAD")
    endif()

    # Guard per directory (not globally) so that each subdirectory
    # (e.g. csvexport, capture) generates its own GitRef.hpp in its own
    # binary dir, but multiple targets within the same subdirectory share
    # the same custom target (avoiding duplicate ninja rules).
    get_directory_property(_git_ref_target DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" _GIT_REF_TARGET)
    if(NOT _git_ref_target)
        find_package(Git)
        set(_git_ref_hpp "${CMAKE_CURRENT_BINARY_DIR}/GitRef.hpp")
        set(_git_ref_tmp "${CMAKE_CURRENT_BINARY_DIR}/GitRef.hpp.tmp")
        string(MAKE_C_IDENTIFIER "git-ref-${CMAKE_CURRENT_BINARY_DIR}" _git_ref_target)
        if(Git_FOUND)
            add_custom_target(${_git_ref_target}
                COMMAND ${CMAKE_COMMAND} -E echo "#pragma once" > ${_git_ref_tmp}
                COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR} log -1 "--format=namespace tracy { static inline const char* GitRef = %x22%h%x22; }" ${GIT_REV} >> ${_git_ref_tmp} || echo "namespace tracy { static inline const char* GitRef = \"unknown\"; }" >> ${_git_ref_tmp}
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_git_ref_tmp} ${_git_ref_hpp}
                BYPRODUCTS ${_git_ref_hpp} ${_git_ref_tmp}
                VERBATIM
            )
            set_directory_properties(PROPERTIES _GIT_REF_TARGET "${_git_ref_target}")
            set_directory_properties(PROPERTIES _GIT_REF_FOUND TRUE)
        else()
            message(WARNING "git not found, using 'unknown' as git ref.")
            add_custom_command(
                OUTPUT ${_git_ref_hpp}
                COMMAND ${CMAKE_COMMAND} -E echo "#pragma once" > ${_git_ref_hpp}
                COMMAND ${CMAKE_COMMAND} -E echo "namespace tracy { static inline const char* GitRef = \"unknown\"; }" >> ${_git_ref_hpp}
                VERBATIM
            )
            set_directory_properties(PROPERTIES _GIT_REF_TARGET "${_git_ref_target}")
            set_directory_properties(PROPERTIES _GIT_REF_FOUND FALSE)
            set_directory_properties(PROPERTIES _GIT_REF_HPP "${_git_ref_hpp}")
        endif()
    endif()

    get_directory_property(_git_ref_found DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" _GIT_REF_FOUND)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    if(_git_ref_found)
        add_dependencies(${target} ${_git_ref_target})
    else()
        get_directory_property(_git_ref_hpp DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" _GIT_REF_HPP)
        target_sources(${target} PUBLIC ${_git_ref_hpp})
    endif()
endfunction()
