add_library(sim STATIC
    sim.cpp
    units.cpp
    path.cpp
    deploy.cpp
    territory.cpp
    front.cpp
    economy.cpp
    supply.cpp
    combat.cpp
    partisan.cpp)
target_include_directories(sim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(sim PUBLIC mapdata EnTT::EnTT)

# Use real standard headers in doctest 2.4.12 to avoid MSVC C5285 in both test targets.
target_compile_definitions(doctest INTERFACE DOCTEST_CONFIG_USE_STD_HEADERS)

add_executable(sim_test "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/sim_test.cpp")
target_link_libraries(sim_test PRIVATE sim doctest::doctest)
add_test(NAME sim_test COMMAND sim_test)
