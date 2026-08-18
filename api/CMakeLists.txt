# 设置 Qt 路径
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_PREFIX_PATH "D:/CZH720/tools/Qt/6.10.3/llvm-mingw_64")
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui)

# 1. 收集 api 目录下所有子目录的源文件
file(GLOB_RECURSE API_SOURCES
        "unilts/*.cpp" "unilts/*.hpp"
        "base/*.cpp" "base/*.hpp"
        "reg/*.cpp" "reg/*.hpp"
        "res/*.cpp" "res/*.hpp"
        "str/*.cpp" "str/*.hpp"
        "ui/*.cpp" "ui/*.hpp"
)

# 2. 将这些源文件编译成一个名为 "api" 的静态库
add_library(api STATIC ${API_SOURCES})

# 3. 设置这个库的头文件搜索路径
target_include_directories(api
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/base
        ${CMAKE_CURRENT_SOURCE_DIR}/reg
        ${CMAKE_CURRENT_SOURCE_DIR}/res
        ${CMAKE_CURRENT_SOURCE_DIR}/str
        ${CMAKE_CURRENT_SOURCE_DIR}/ui
        ${CMAKE_CURRENT_SOURCE_DIR}/unilts
)

# 4. 链接 Qt6::Widgets 库 (新增这一行)
target_link_libraries(api PUBLIC 
        Qt6::Core
        Qt6::Widgets
        Qt6::Gui
)