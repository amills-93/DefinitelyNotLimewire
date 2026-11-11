// common.h
// Shared helper functions used by the tracker and the peer

#ifndef COMMON_H
#define COMMON_H

#include <string>
#include <vector>

// Reads a config file and returns its non-empty lines, in order.
// Returns an empty list if the file can't be opened.
std::vector<std::string> readConfigLines(const std::string& path);

#endif