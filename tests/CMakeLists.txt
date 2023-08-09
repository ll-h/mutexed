find_package(Boost 1.82 COMPONENTS unit_test_framework REQUIRED)

add_executable(mutexed_tests mutexed.cpp)
set_target_properties(mutexed_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)
target_include_directories(mutexed_tests PUBLIC ${Boost_INCLUDE_DIRS}) 
target_include_directories(mutexed_tests PUBLIC ${CMAKE_SOURCE_DIR}/include/llh)
target_link_libraries(mutexed_tests ${Boost_LIBRARIES})

add_test(NAME Mutexed COMMAND mutexed_tests -l test_suite)
