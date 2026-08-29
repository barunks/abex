cmake_minimum_required(VERSION 3.24)
cmake_policy(SET CMP0167 OLD)

project(abex_gateway VERSION 0.1.0 LANGUAGES CXX)

option(ABEX_BUILD_TESTS "Build the automated test suite" ON)
option(ABEX_BUILD_SERVER "Build the REST/WebSocket server and browser UI host" ON)
option(ABEX_BUILD_BENCHMARKS "Build the standalone hot-path benchmark" OFF)
option(ABEX_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(nlohmann_json 3.11 REQUIRED)
find_package(Threads REQUIRED)
find_package(Boost 1.81 REQUIRED COMPONENTS system)
find_package(OpenSSL 3 REQUIRED)
set(CURL_LIBRARY "/usr/lib/x86_64-linux-gnu/libcurl.so")
set(CURL_INCLUDE_DIR "/usr/include")
find_package(CURL 7 REQUIRED)
find_package(yaml-cpp REQUIRED)

add_library(abex_core
    src/domain/decimal.cpp
    src/domain/operational_event.cpp
    src/domain/order.cpp
    src/application/risk_manager.cpp
    src/application/market_data_book.cpp
    src/application/order_gateway.cpp
    src/infrastructure/file_order_store.cpp
    src/infrastructure/simulated_exchange_adapter.cpp
    src/infrastructure/exchange_protocols.cpp
    src/infrastructure/crypto.cpp
    src/infrastructure/http_client.cpp
    src/infrastructure/market_data_ring.cpp
    src/infrastructure/reconnecting_websocket.cpp
    src/infrastructure/okx_adapter.cpp
    src/infrastructure/binance_adapter.cpp
    src/cli/command_processor.cpp
    src/presentation/json_views.cpp
    src/server/gateway_api.cpp
    src/bootstrap/environment.cpp
)

target_include_directories(abex_core PUBLIC include)
target_link_libraries(abex_core PUBLIC
    nlohmann_json::nlohmann_json Threads::Threads
    OpenSSL::SSL OpenSSL::Crypto CURL::libcurl Boost::system yaml-cpp
)
target_compile_features(abex_core PUBLIC cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(abex_core PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor
        -Wno-missing-field-initializers
    )
    set_source_files_properties(src/domain/decimal.cpp PROPERTIES COMPILE_OPTIONS -Wno-pedantic)
endif()

if(ABEX_ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(abex_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(abex_core PUBLIC -fsanitize=address,undefined)
endif()

if(ABEX_BUILD_SERVER)
    add_library(abex_server_lib src/server/http_server.cpp)
    target_include_directories(abex_server_lib PUBLIC include)
    target_link_libraries(abex_server_lib PUBLIC abex_core Boost::system Threads::Threads)
    target_compile_features(abex_server_lib PUBLIC cxx_std_20)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(abex_server_lib PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor
            -Wno-missing-field-initializers
        )
    endif()
endif()

add_executable(abex_cli apps/abex_cli/main.cpp)
target_link_libraries(abex_cli PRIVATE abex_core)

add_executable(abex_market_data apps/abex_market_data/main.cpp)
target_link_libraries(abex_market_data PRIVATE abex_core)

if(ABEX_BUILD_SERVER)
    add_executable(abex_server apps/abex_server/main.cpp)
    target_link_libraries(abex_server PRIVATE abex_server_lib)
    add_dependencies(abex_server abex_market_data)
endif()

install(TARGETS abex_cli abex_market_data RUNTIME DESTINATION bin)
if(ABEX_BUILD_SERVER)
    install(TARGETS abex_server RUNTIME DESTINATION bin)
    install(DIRECTORY web/ DESTINATION share/abex/web)
endif()
install(FILES config/gateway.example.json DESTINATION share/abex/config)

if(ABEX_BUILD_TESTS)
    include(CTest)
    find_package(Catch2 3 REQUIRED)

    add_executable(abex_tests
        tests/test_decimal.cpp
        tests/test_order_state_machine.cpp
        tests/test_risk_manager.cpp
        tests/test_sequence_tracker.cpp
        tests/test_protocols.cpp
        tests/test_gateway.cpp
        tests/test_gateway_api.cpp
        tests/test_recovery.cpp
        tests/test_properties.cpp
        tests/test_environment.cpp
        tests/test_market_data.cpp
        tests/test_rate_limiter.cpp
        tests/test_fault_tolerance.cpp
        tests/test_crash_recovery.cpp
    )
    target_link_libraries(abex_tests PRIVATE abex_core Catch2::Catch2WithMain)

    if(ABEX_BUILD_SERVER)
        target_sources(abex_tests PRIVATE tests/test_http_server.cpp)
        target_link_libraries(abex_tests PRIVATE abex_server_lib)
        target_compile_definitions(abex_tests PRIVATE ABEX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    include(Catch)
    catch_discover_tests(abex_tests)
endif()

if(ABEX_BUILD_BENCHMARKS)
    add_executable(abex_benchmark benchmarks/hot_paths.cpp)
    target_link_libraries(abex_benchmark PRIVATE abex_core)
endif()
