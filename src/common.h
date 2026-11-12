// common.h
// Shared helper functions used by the tracker and the peer

#ifndef COMMON_H
#define COMMON_H

#include <string>
#include <vector>

// Reads a config file and returns its non-empty lines, in order, with
// leading/trailing whitespace removed.
// Returns an empty list if the file can't be opened.
std::vector<std::string> readConfigLines(const std::string& path);

// Reads a config file into lines like readConfigLines, but tells a
// missing/unreadable file apart from a short one. On failure it prints a
// clear error that names the file and
// what it must contain (usage), and returns false.
bool loadConfigFile(const std::string& who, const std::string& path,
                    std::size_t minLines, const std::string& usage,
                    std::vector<std::string>& lines);

// Parses a whole-string decimal number in [minValue, maxValue].
// Returns false for empty text, junk characters, or out-of-range values.
bool parseIntInRange(const std::string& text, long minValue, long maxValue, int& value);

// Parses a TCP port number (1-65535).
bool parsePort(const std::string& text, int& port);

#endif
