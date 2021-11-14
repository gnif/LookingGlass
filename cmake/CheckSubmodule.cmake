if (EXISTS "${PROJECT_TOP}/.git" AND (
    (NOT EXISTS "${PROJECT_TOP}/repos/cimgui/.git") OR
    (NOT EXISTS "${PROJECT_TOP}/repos/LGMP/.git") OR
    (NOT EXISTS "${PROJECT_TOP}/repos/PureSpice/.git") OR
    (NOT EXISTS "${PROJECT_TOP}/repos/cimgui/imgui/.git")
))
    message(FATAL_ERROR "Submodules are not initialized. Run\n\tgit submodule update --init --recursive")
endif()

if (EXISTS "${PROJECT_TOP}/.git" AND NOT DEFINED DEVELOPER)
    execute_process(
        COMMAND git submodule summary
        WORKING_DIRECTORY "${PROJECT_TOP}"
        OUTPUT_VARIABLE SUBMODULE_SUMMARY
    )
    if (NOT "${SUBMODULE_SUMMARY}" STREQUAL "")
       message(FATAL_ERROR "Wrong submodule version checked out. Run\n\tgit submodule update --init --recursive")
    endif()
endif()
