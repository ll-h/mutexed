cmake_minimum_required(VERSION 3.11)

project(mutexed)

include(CTest)

find_package(Boost)

add_library(${PROJECT_NAME} INTERFACE)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
target_include_directories(${PROJECT_NAME}
    INTERFACE ${Boost_INCLUDE_DIRS}
    INTERFACE ${PROJECT_SOURCE_DIR}/include
) 

enable_testing()
add_subdirectory(tests)
