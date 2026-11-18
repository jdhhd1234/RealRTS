# app: Raylib 6.0 창·입력·렌더링 + raygui 패널 + tinyfiledialogs 파일 대화상자. sim은 읽기와 SimCommand만.
enable_language(C)  # raylib, tinyfiledialogs are C
include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 6.0
    GIT_SHALLOW TRUE)
FetchContent_Declare(raygui
    GIT_REPOSITORY https://github.com/raysan5/raygui.git
    GIT_TAG 5.0
    GIT_SHALLOW TRUE)
FetchContent_Declare(tinyfiledialogs
    GIT_REPOSITORY https://github.com/native-toolkit/libtinyfiledialogs.git
    GIT_TAG cc6b593c029110af8045826ce691f540c85e850c)
FetchContent_MakeAvailable(raylib)
if(MSVC)
    target_compile_options(raylib PRIVATE /W0)  # third-party; our /W4 is for our code
endif()
FetchContent_Populate(raygui)  # 헤더만 쓴다 (CMake 타깃 없음)
FetchContent_Populate(tinyfiledialogs)

add_library(tinyfd STATIC ${tinyfiledialogs_SOURCE_DIR}/tinyfiledialogs.c)
target_include_directories(tinyfd PUBLIC ${tinyfiledialogs_SOURCE_DIR})
if(MSVC)
    target_compile_options(tinyfd PRIVATE /W0)
endif()

# 화면과 무관한 로직(편집기, 파일 읽기)은 app_core로 분리해 테스트에서 링크한다.
add_library(app_core STATIC editor.cpp files.cpp)
target_include_directories(app_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(app_core PUBLIC sim mapbake tinyfd)

add_executable(app main.cpp ui.cpp symbols.cpp raygui_impl.cpp)
target_include_directories(app PRIVATE ${raygui_SOURCE_DIR}/src)
target_link_libraries(app PRIVATE app_core raylib)
target_compile_definitions(app PRIVATE ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")

add_executable(app_test ${PROJECT_SOURCE_DIR}/tests/app_test.cpp ${PROJECT_SOURCE_DIR}/tests/app_symbols_test.cpp)
target_link_libraries(app_test PRIVATE app_core raylib doctest::doctest)
target_compile_definitions(app_test PRIVATE ASSETS_DIR="${PROJECT_SOURCE_DIR}/assets")
add_test(NAME app_test COMMAND app_test)
