if (EXISTS "${PROJECT_TOP}/.git" AND (
    (NOT EXISTS "${PROJECT_TOP}/repos/cimgui/.git") OR
    (NOT EXISTS "${PROJECT_TOP}/repos/LGMP/.git") OR
    (NOT EXISTS "${PROJECT_TOP}/repos/PureSpice/.git") OR
    (NOT EXISTS "${PROJECT_TOP}/repos/cimgui/imgui/.git")
))
    message(FATAL_ERROR "Submodules are not initialized. Run\n\tgit submodule update --init --recursive")
endif()
