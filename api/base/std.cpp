#include "std.hpp"

bool func::similarly(int x, const std::vector<int>& l) {
    for (const int& item : l) {
        if (x == item) return true;
    }
    return false;
}
