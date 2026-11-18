cmake_minimum_required(VERSION 3.24)
project(RealRTS LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 결정론(architect.md 11장 B3): 부동소수점 최적화 방식 고정.
if(MSVC)
    add_compile_options(/W4 /utf-8 /fp:precise)
else()
    add_compile_options(-Wall -Wextra -ffp-contract=off)
endif()

include(FetchContent)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.12)
FetchContent_Declare(EnTT
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.15.0)
FetchContent_MakeAvailable(nlohmann_json doctest EnTT)

enable_testing()

# mapdata: 정적 지도 데이터 (렌더링·ECS 의존 없음)
add_library(mapdata STATIC src/mapdata/mapdata.cpp)
target_include_directories(mapdata PUBLIC src)
target_link_libraries(mapdata PUBLIC nlohmann_json::nlohmann_json)

add_executable(mapdata_test tests/mapdata_test.cpp)
target_link_libraries(mapdata_test PRIVATE mapdata doctest::doctest)
add_test(NAME mapdata_test COMMAND mapdata_test)

# sim: 시뮬레이션 코어 (EnTT + mapdata만 의존, raylib 금지). 타깃 정의는 src/sim/CMakeLists.txt.
add_subdirectory(src/sim)

# mapbake: GPKG/SHP/GeoJSON -> mapdata::Baked (SQLite + shapelib). 타깃 정의는 src/mapbake/CMakeLists.txt.
add_subdirectory(src/mapbake)

# app: Raylib 6.0 + raygui + tinyfiledialogs. 타깃 정의는 src/app/CMakeLists.txt.
add_subdirectory(src/app)
