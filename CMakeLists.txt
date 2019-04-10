cmake_minimum_required(VERSION 3.8.0)
project(fastbeast VERSION 0.1.0 LANGUAGES CXX)

find_package(Boost 1.69
             COMPONENTS
                system
                coroutine
             REQUIRED)
find_package(ufiber REQUIRED)
find_package (Threads REQUIRED)

# add_library(prebuilt-asio OBJECT prebuilt_asio.cpp)
# target_compile_definitions(prebuilt-asio PUBLIC
#     BOOST_ASIO_SEPARATE_COMPILATION
#     BOOST_BEAST_SPLIT_COMPILATION
# )

# target_compile_features(prebuilt-asio PUBLIC cxx_std_17)

# target_link_libraries(prebuilt-asio PUBLIC
#     Threads::Threads
#     Boost::boost
# )

add_executable(fastbeast-app
    main.cpp
    log.cpp
    http_session.cpp)

target_compile_features(fastbeast-app PUBLIC cxx_std_17)

target_link_libraries(fastbeast-app PUBLIC
    ufiber::ufiber
)
