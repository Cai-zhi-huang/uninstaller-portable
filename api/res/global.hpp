//
// Created by xubow on 2026/7/20.
//

#ifndef UNINSTALLER_GLABE_H
#define UNINSTALLER_GLABE_H
#include "constexpr.hpp"
#include "std.hpp"

inline struct global {
    // The language option. 0: us-en, 1: zh-cn
    short LANGUAGE = 1;
    // Windows size
    std::array<short, 2> WINDOWS_SIZE{1200, 600};
} G;


#endif //UNINSTALLER_GLABE_H
