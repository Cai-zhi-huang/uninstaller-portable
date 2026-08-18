cmake_minimum_required(VERSION 3.20)
set(PROJECT_NAME "uninstaller")

# 工具链（必须在 project() 之前设置，否则编译器会在 project() 阶段被锁定为 PATH 中的 g++）
set(TOOLCHAIN_BIN_DIR "D:/CZH720/tools/Qt/Tools/llvm-mingw1706_64/bin")
set(CMAKE_C_COMPILER    "${TOOLCHAIN_BIN_DIR}/clang.exe")
set(CMAKE_CXX_COMPILER  "${TOOLCHAIN_BIN_DIR}/clang++.exe")
set(CMAKE_RC_COMPILER   "${TOOLCHAIN_BIN_DIR}/llvm-rc.exe")
set(CMAKE_AR            "${TOOLCHAIN_BIN_DIR}/llvm-ar.exe")
set(CMAKE_RANLIB        "${TOOLCHAIN_BIN_DIR}/llvm-ranlib.exe")
set(CMAKE_MAKE_PROGRAM  "D:/CZH720/tools/Qt/Tools/Ninja/ninja.exe" CACHE FILEPATH "Ninja build tool" FORCE)

# 1. 强制指定使用 LLD 链接器（必须在 project() 之前设置，确保全局生效）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_EXE_LINKER_FLAGS    "${CMAKE_EXE_LINKER_FLAGS} -fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fuse-ld=lld")

project(${PROJECT_NAME} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 设置 Qt 路径
set(CMAKE_PREFIX_PATH "D:/CZH720/tools/Qt/6.10.3/llvm-mingw_64")

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui)

# 添加子目录
add_subdirectory(api)

# 定义主程序（附带 Windows 资源文件，包含程序图标）
add_executable(${PROJECT_NAME} WIN32 main.cpp appicon.rc assets/appicon.qrc lang/lang.qrc)

# 链接库
target_link_libraries(${PROJECT_NAME} PRIVATE
        api
        Qt6::Core
        Qt6::Widgets
        Qt6::Gui
        advapi32
        shell32
)

# --- 修正 Windows 子系统判断逻辑 ---
# Clang on Windows (MinGW) 既不是 MSVC 也不是 GNUCXX (它是 Clang)
# 所以原来的 elseif(CMAKE_COMPILER_IS_GNUCXX) 对 Clang 无效
if(WIN32)
    # 无论是 MSVC 还是 Clang-MinGW，都推荐使用 /SUBSYSTEM:WINDOWS
    # Clang-cl 支持 /SUBSYSTEM，Clang-MinGW 也支持通过链接器传递该参数
    if(MSVC)
        target_link_options(${PROJECT_NAME} PRIVATE /SUBSYSTEM:WINDOWS)
    else()
        # 对于 MinGW/Clang，使用 -mwindows 是最稳妥的方式
        # 它会自动处理子系统设置并链接必要的 GUI 库
        target_link_options(${PROJECT_NAME} PRIVATE -mwindows)
    endif()
endif()

# 部署 Qt DLL
find_program(WINDEPLOYQT windeployqt HINTS "${CMAKE_PREFIX_PATH}/bin")
if(WINDEPLOYQT)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${WINDEPLOYQT} --no-translations "$<TARGET_FILE_DIR:${PROJECT_NAME}>/${PROJECT_NAME}.exe"
            COMMENT "Deploying Qt runtime dependencies..."
    )
else()
    message(WARNING "windeployqt not found! Qt dependencies will be missing.")
endif()

# 部署语言数据文件到 exe 同级 lang/，可被外部文件覆盖（新增语言无需重新编译）
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${PROJECT_NAME}>/lang"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/lang/languages.json"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/lang/languages.json"
        COMMENT "Deploying language data file (lang/languages.json)..."
)