#pragma once
// created by: xu.bw 26.7.5
// Include all basic standard headers and define common type aliases and namespaces for convenience.
// This header file is designed to be included in other parts of the project to provide access to standard library features and utility functions.

#include <map>
#include <set>
#include <array>
#include <string>
#include <vector>
#include <format>
#include <cstdint>
#include <fstream>
#include <cstdint>
#include <utility>
#include <iostream>
#include <shlobj.h>
#include <fileapi.h>
#include <algorithm>
#include <windows.h>
#include <filesystem>
#include <string_view>
#if __has_include(<filesystem>)
    #include <filesystem>
    namespace fs = std::filesystem;
#elif __has_include(<experimental/filesystem>)
    #include <experimental/filesystem>
    namespace fs = std::experimental::filesystem;
#else
    #error "当前编译器不支持任何版本的 filesystem 库"
#endif


// typenames
using uint = unsigned int;
using ll = long long;
using ull = unsigned long long;
using ld = long double;

// mingw64
using i128 = long long;
using ui128 = unsigned long long;


// spacename
//using namespace std;

namespace func {
    bool similarly(int x, const std::vector<int>& l);
}
