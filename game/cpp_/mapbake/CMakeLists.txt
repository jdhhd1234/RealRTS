enable_language(C)

if(MSVC)
    get_directory_property(_dir_opts COMPILE_OPTIONS)
    list(REMOVE_ITEM _dir_opts "/W4")
    list(APPEND _dir_opts "$<$<COMPILE_LANGUAGE:CXX>:/W4>" "$<$<COMPILE_LANGUAGE:C>:/W0>")
    set_directory_properties(PROPERTIES COMPILE_OPTIONS "${_dir_opts}")
    if(TARGET doctest)
        target_compile_options(doctest INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:/wd5285>")
    endif()
endif()

FetchContent_Declare(sqlite3
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3470200.zip)
FetchContent_Declare(shapelib
    GIT_REPOSITORY https://github.com/OSGeo/shapelib.git
    GIT_TAG v1.6.1
    SOURCE_SUBDIR _nonexistent_)
FetchContent_MakeAvailable(sqlite3 shapelib)

add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 SYSTEM PUBLIC ${sqlite3_SOURCE_DIR})
if(NOT MSVC)
    target_compile_options(sqlite3 PRIVATE -w)
endif()

add_library(shapelib STATIC
    ${shapelib_SOURCE_DIR}/shpopen.c
    ${shapelib_SOURCE_DIR}/dbfopen.c
    ${shapelib_SOURCE_DIR}/safileio.c
    ${shapelib_SOURCE_DIR}/shptree.c)
target_include_directories(shapelib SYSTEM PUBLIC ${shapelib_SOURCE_DIR})
if(NOT MSVC)
    target_compile_options(shapelib PRIVATE -w)
endif()

add_library(mapbake STATIC
    mapbake.cpp
    geom.cpp
    folder.cpp)
target_include_directories(mapbake PUBLIC ${CMAKE_SOURCE_DIR}/cpp_)
target_link_libraries(mapbake PUBLIC mapdata sqlite3 shapelib)

add_executable(mapbake_cli main.cpp)
target_link_libraries(mapbake_cli PRIVATE mapbake)

add_executable(mapbake_test ${CMAKE_SOURCE_DIR}/tests/mapbake_test.cpp)
target_link_libraries(mapbake_test PRIVATE mapbake doctest::doctest)
target_compile_definitions(mapbake_test PRIVATE MAPBAKE_GAME_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME mapbake_test COMMAND mapbake_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
