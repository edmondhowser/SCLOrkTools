set(CMAKE_CXX_STANDARD 17)

if (BUILD_DOC)
find_package(Doxygen)
    if (DOXYGEN_FOUND)
        # Preprocess Doxyfile to resolve CMake path variables within
        set(DOXYGEN_IN ${CMAKE_CURRENT_SOURCE_DIR}/doc/Doxyfile.in)
        set(DOXYGEN_OUT ${CMAKE_CURRENT_BINARY_DIR}/doc/Doxyfile)
        configure_file(${DOXYGEN_IN} ${DOXYGEN_OUT} @ONLY)
        message("Configuring Doxygen build.")

        add_custom_target(docs ALL
            COMMAND ${DOXYGEN_EXECUTABLE} ${DOXYGEN_OUT}
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating API documentation with Doxygen"
            VERBATIM)

    else (DOXYGEN_FOUND)
        message("Doxygen not found, skipping documentation build.")
    endif (DOXYGEN_FOUND)
endif (BUILD_DOC)

# Allow other projects to include the Common library headers.
include_directories(${CMAKE_CURRENT_LIST_DIR}/common/include)

set(CMAKE_CXX_FLAGS "-g")

add_subdirectory(common)
add_subdirectory(confab)

