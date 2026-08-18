#include "struct.hpp"

filesize_t::filesize_t(ld _t): size(_t){
    this->format_size=formatS();
}

std::string filesize_t::formatS(){
    int unitIndex = 0;
    ld fileSize = size;
    while (fileSize >= 1024 && unitIndex < SIZEUNITESLEN - 1) {
        fileSize /= 1024;
        unitIndex++;
    }
    return std::format("{:.2f} {}", fileSize, SIZEUNITS[unitIndex]);
}

void filesize_t::formatN(){
    this->format_size = formatS();
}

std::string filesize_t::get(){
    return this->format_size;
};

filesize_t& filesize_t::operator=(ld& _t){
    this->size = _t;
    formatN();
    return *this;
}

filesize_t& filesize_t::operator+=(ld& _t){
    this->size += _t;
    formatN();
    return *this;
}
