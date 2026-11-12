// common.cpp
// Code for the shared helper functions listed in common.h.

#include "common.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

namespace {

// Removes leading and trailing whitespace (including a Windows "\r")
std::string trim(const std::string& text) {
    std::size_t start = 0;
    std::size_t end = text.size();
    while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) {
        start++;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        end--;
    }
    return text.substr(start, end - start);
}

// Reads the non-blank, trimmed lines of an already opened file
std::vector<std::string> readLines(std::ifstream& file) {
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}

}  // namespace

std::vector<std::string> readConfigLines(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return {};
    }
    return readLines(file);
}

bool loadConfigFile(const std::string& who, const std::string& path,
                    std::size_t minLines, const std::string& usage,
                    std::vector<std::string>& lines) {
    std::ifstream file(path);
    if (!file) {
        std::cerr << who << ": cannot open config file '" << path << "': "
                  << std::strerror(errno) << "\n"
                  << who << ": it must be in the directory the program is run from and contain:\n"
                  << usage;
        return false;
    }
    lines = readLines(file);
    if (lines.size() < minLines) {
        std::cerr << who << ": config file '" << path << "' is incomplete (found "
                  << lines.size() << " non-blank line(s), need at least " << minLines
                  << "). Expected:\n"
                  << usage;
        return false;
    }
    return true;
}

bool parseIntInRange(const std::string& text, long minValue, long maxValue, int& value) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    long parsed = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || *end != '\0' || parsed < minValue || parsed > maxValue) {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

bool parsePort(const std::string& text, int& port) {
    return parseIntInRange(text, 1, 65535, port);
}
