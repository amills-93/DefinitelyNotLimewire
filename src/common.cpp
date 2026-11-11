// common.cpp
// Code for the shared helper functions listed in common.h.

#include "common.h"

#include <fstream>

std::vector<std::string> readConfigLines(const std::string& path) {
    std::vector<std::string> lines;   // the list to fill and return
    std::ifstream file(path);         // open the file for reading
    std::string line;                 // holds one line at a time

    // Read the file one line at a time until end
    while (std::getline(file, line)) {
        // Removes "\r" character
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // Skip blank lines
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    return lines;
}