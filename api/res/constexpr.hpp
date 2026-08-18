//
// Created by xubow on 2026/7/20.
//

#ifndef UNINSTALLER_CONSTEXPR_H
#define UNINSTALLER_CONSTEXPR_H

#include "std.hpp"

// Const int exprcoding
constexpr short SOFTWARETYPE{5};        //Application type size.
constexpr short SIZEUNITESLEN{6};       //Size types.
constexpr short OSERRORTYPES{10};

// Const string exprcoding
constexpr std::array<const char *, SOFTWARETYPE> SWSORTS{
    "Normal",
    "WindowsInstaller",
    "SystemComponent",
    "RunningTime",
    "Unknown"
};// Application type and size.
constexpr std::array<const char *,SIZEUNITESLEN> SIZEUNITS {
    "B", "KB", "MB", "GB", "TB", "PB"
};// File size format units set;
#endif //UNINSTALLER_CONSTEXPR_H
