# Runs hello_dump and compares its output byte for byte with the fixture.
# With -DREFRESH=1 it rewrites the fixture instead.
# Inputs: -DDUMP=<hello_dump path> -DFIXTURE=<fixture path>

execute_process(COMMAND ${DUMP} OUTPUT_VARIABLE actual RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "hello_dump exited with ${rc}")
endif()

if(REFRESH)
  file(WRITE ${FIXTURE} "${actual}")
  message(STATUS "rewrote ${FIXTURE}")
  return()
endif()

file(READ ${FIXTURE} expected)
if(NOT actual STREQUAL expected)
  message(FATAL_ERROR "${FIXTURE} is stale. Run: cmake --build build --target hello_fixture_refresh")
endif()
