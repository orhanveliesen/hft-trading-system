# Code Coverage Configuration
# This module provides code coverage support using gcov/lcov

option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)

if(ENABLE_COVERAGE)
    # Check if we're using a compatible compiler
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(WARNING "Code coverage requires GCC or Clang compiler")
        return()
    endif()

    # Check for required tools
    find_program(LCOV_PATH lcov)
    find_program(GENHTML_PATH genhtml)

    if(NOT LCOV_PATH)
        message(WARNING "lcov not found! Install with: apt-get install lcov")
    endif()

    if(NOT GENHTML_PATH)
        message(WARNING "genhtml not found! Install with: apt-get install lcov")
    endif()

    # Add coverage flags to compiler
    set(COVERAGE_FLAGS "--coverage -fprofile-arcs -ftest-coverage -O0 -g")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COVERAGE_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COVERAGE_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")

    message(STATUS "Code coverage enabled")
    message(STATUS "  - Coverage flags: ${COVERAGE_FLAGS}")
    message(STATUS "  - lcov: ${LCOV_PATH}")
    message(STATUS "  - genhtml: ${GENHTML_PATH}")

    # Add custom target for generating coverage reports
    if(LCOV_PATH AND GENHTML_PATH)
        add_custom_target(coverage
            COMMAND ${CMAKE_COMMAND} -E echo "Running tests with coverage..."
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure || true

            COMMAND ${CMAKE_COMMAND} -E echo "Capturing coverage data..."
            COMMAND ${LCOV_PATH} --capture --directory . --output-file coverage.info

            COMMAND ${CMAKE_COMMAND} -E echo "Filtering coverage data..."
            COMMAND ${LCOV_PATH} --remove coverage.info '/usr/*' '*/external/*' '*/tests/*' --output-file coverage_filtered.info

            COMMAND ${CMAKE_COMMAND} -E echo "Coverage summary:"
            COMMAND ${LCOV_PATH} --list coverage_filtered.info

            COMMAND ${CMAKE_COMMAND} -E echo "Generating HTML report..."
            COMMAND ${GENHTML_PATH} coverage_filtered.info --output-directory coverage_html

            COMMAND ${CMAKE_COMMAND} -E echo "Coverage report generated in coverage_html/index.html"

            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Generating code coverage report"
        )

        message(STATUS "  - Run 'make coverage' to generate coverage report")
    endif()
endif()
