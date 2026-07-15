if(NOT DEFINED SOURCE_SOLUTION OR NOT DEFINED OUTPUT_SOLUTION OR
   NOT DEFINED BUILD_RELATIVE)
    message(FATAL_ERROR "CreateRootSolution requires SOURCE_SOLUTION, OUTPUT_SOLUTION, and BUILD_RELATIVE")
endif()

if(NOT EXISTS "${SOURCE_SOLUTION}")
    message(FATAL_ERROR "Generated solution was not found: ${SOURCE_SOLUTION}")
endif()

string(REPLACE "/" "\\" BUILD_PREFIX "${BUILD_RELATIVE}")
file(READ "${SOURCE_SOLUTION}" GENERATED_SOLUTION)

set(PROJECT_NAMES Engine Editor Game)
set(CURATED_PROJECTS "")
set(CURATED_CONFIGURATIONS "")
set(FOUND_PROJECT_COUNT 0)

foreach(PROJECT_NAME IN LISTS PROJECT_NAMES)
    string(REGEX MATCH
        "Project\\(\"([^\"]+)\"\\) = \"${PROJECT_NAME}\", \"([^\"]+\\.vcxproj)\", \"([^\"]+)\""
        PROJECT_DECLARATION "${GENERATED_SOLUTION}")
    if(NOT PROJECT_DECLARATION)
        continue()
    endif()

    set(PROJECT_TYPE "${CMAKE_MATCH_1}")
    set(PROJECT_PATH "${CMAKE_MATCH_2}")
    set(PROJECT_GUID "${CMAKE_MATCH_3}")
    string(APPEND CURATED_PROJECTS
        "Project(\"${PROJECT_TYPE}\") = \"${PROJECT_NAME}\", \"${BUILD_PREFIX}\\${PROJECT_PATH}\", \"${PROJECT_GUID}\"\n"
        "EndProject\n")

    foreach(CONFIGURATION Debug Release MinSizeRel RelWithDebInfo)
        string(APPEND CURATED_CONFIGURATIONS
            "\t\t${PROJECT_GUID}.${CONFIGURATION}|x64.ActiveCfg = ${CONFIGURATION}|x64\n"
            "\t\t${PROJECT_GUID}.${CONFIGURATION}|x64.Build.0 = ${CONFIGURATION}|x64\n")
    endforeach()
    math(EXPR FOUND_PROJECT_COUNT "${FOUND_PROJECT_COUNT} + 1")
endforeach()

if(NOT FOUND_PROJECT_COUNT EQUAL 3)
    message(FATAL_ERROR "Could not find Engine, Editor, and Game in ${SOURCE_SOLUTION}")
endif()

string(CONCAT ROOT_SOLUTION_CONTENT
    "Microsoft Visual Studio Solution File, Format Version 12.00\n"
    "# Visual Studio Version 17\n"
    "VisualStudioVersion = 17.0.31903.59\n"
    "MinimumVisualStudioVersion = 10.0.40219.1\n"
    "${CURATED_PROJECTS}"
    "Global\n"
    "\tGlobalSection(SolutionConfigurationPlatforms) = preSolution\n"
    "\t\tDebug|x64 = Debug|x64\n"
    "\t\tRelease|x64 = Release|x64\n"
    "\t\tMinSizeRel|x64 = MinSizeRel|x64\n"
    "\t\tRelWithDebInfo|x64 = RelWithDebInfo|x64\n"
    "\tEndGlobalSection\n"
    "\tGlobalSection(ProjectConfigurationPlatforms) = postSolution\n"
    "${CURATED_CONFIGURATIONS}"
    "\tEndGlobalSection\n"
    "\tGlobalSection(SolutionProperties) = preSolution\n"
    "\t\tHideSolutionNode = FALSE\n"
    "\tEndGlobalSection\n"
    "EndGlobal\n")

file(WRITE "${OUTPUT_SOLUTION}" "${ROOT_SOLUTION_CONTENT}")
message(STATUS "Curated root solution updated: ${OUTPUT_SOLUTION}")
