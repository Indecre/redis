cmake_minimum_required(VERSION 3.22)

project(KeyForge VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions)
find_package(Threads REQUIRED)

add_library(keyforge_core
    src/common/version.cpp
    src/protocol/resp.cpp
    src/server/config.cpp
    src/server/server.cpp
    src/storage/sorted_set.cpp
    src/storage/database.cpp
    src/graph/dependency_graph.cpp
    src/commands/engine.cpp
    src/persistence/aof.cpp
)
add_library(KeyForge::core ALIAS keyforge_core)
target_compile_features(keyforge_core PUBLIC cxx_std_20)
target_include_directories(keyforge_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(keyforge_core PUBLIC Threads::Threads)
keyforge_apply_project_options(keyforge_core)
if(KEYFORGE_ENABLE_FUZZING)
    target_compile_options(keyforge_core PRIVATE -fsanitize=fuzzer-no-link,address,undefined)
endif()

add_executable(keyforge-server src/main.cpp)
target_link_libraries(keyforge-server PRIVATE KeyForge::core)
keyforge_apply_project_options(keyforge-server)

add_executable(keyforge-cli apps/cli/main.cpp)
target_link_libraries(keyforge-cli PRIVATE KeyForge::core)
keyforge_apply_project_options(keyforge-cli)

if(KEYFORGE_ENABLE_TESTS)
    include(CTest)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    add_executable(keyforge-tests
        tests/unit/bootstrap_test.cpp
        tests/unit/resp_test.cpp
        tests/unit/engine_test.cpp
        tests/integration/server_test.cpp
        tests/recovery/aof_test.cpp
    )
    target_link_libraries(keyforge-tests PRIVATE KeyForge::core Catch2::Catch2WithMain)
    target_compile_definitions(keyforge-tests PRIVATE
        KEYFORGE_SERVER_PATH="$<TARGET_FILE:keyforge-server>"
    )
    add_dependencies(keyforge-tests keyforge-server)
    keyforge_apply_project_options(keyforge-tests)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    include(Catch)
    catch_discover_tests(keyforge-tests DISCOVERY_MODE PRE_TEST)
endif()

if(KEYFORGE_ENABLE_FUZZING)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "KEYFORGE_ENABLE_FUZZING requires Clang")
    endif()
    add_executable(keyforge-resp-fuzz fuzz/resp_parser_fuzz.cpp)
    target_link_libraries(keyforge-resp-fuzz PRIVATE KeyForge::core)
    target_compile_options(keyforge-resp-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(keyforge-resp-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    keyforge_apply_project_options(keyforge-resp-fuzz)
endif()
