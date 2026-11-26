# Tool tests (label tools): CTest test tools.unittest runs the unittest suite of tests/tools, which
# checks tools/migrate_schemas_to_2020_12.py: that the committed runtime-owned schemas are what the
# script writes (a draft-07 spelling or a hand edit that the script would change fails), and what
# each of its rules does on small schemas. The script and the tests use only the standard library.
# The test needs an interpreter (Python 3.10 or newer); without one it is not registered and the
# configure step says so (the CI runners have one). PYTHONDONTWRITEBYTECODE keeps Python from
# writing bytecode beside the tests.
find_package(Python3 3.10 QUIET COMPONENTS Interpreter)
if(TARGET Python3::Interpreter)
    add_test(NAME tools.unittest
        COMMAND "$<TARGET_FILE:Python3::Interpreter>" -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}" -v)
    set_tests_properties(tools.unittest PROPERTIES
        LABELS tools
        TIMEOUT 120
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
else()
    message(STATUS "Praenora: no Python 3.10 interpreter found; tools.unittest is not registered")
endif()
