cmake_minimum_required(VERSION 3.20)
project(vauth-agent-example LANGUAGES CXX)

find_package(sdbus-c++ CONFIG REQUIRED)

add_executable(vauth-agent-example agent.cpp)
target_compile_features(vauth-agent-example PRIVATE cxx_std_20)
target_link_libraries(vauth-agent-example PRIVATE SDBusCpp::sdbus-c++)

include(CTest)
if(BUILD_TESTING)
    add_executable(agent_example_tests agent_test.cpp)
    target_compile_features(agent_example_tests PRIVATE cxx_std_20)
    target_compile_definitions(agent_example_tests PRIVATE VAUTH_AGENT_EXAMPLE_TESTING)
    target_link_libraries(agent_example_tests PRIVATE SDBusCpp::sdbus-c++)
    add_test(NAME agent_example COMMAND agent_example_tests)
endif()
