include_directories(${CMAKE_CURRENT_SOURCE_DIR})

set(TESTUTIL_SOURCES
    testutil.cpp)

set(TESTUTIL_HEADERS
    testutil.h)

add_library(testutil SHARED ${TESTUTIL_SOURCES} ${TESTUTIL_HEADERS})
target_compile_definitions(testutil PRIVATE -DBUILDING_TESTUTIL)

add_subdirectory(buslogic)
add_subdirectory(client)
add_subdirectory(events)
add_subdirectory(serialization)
