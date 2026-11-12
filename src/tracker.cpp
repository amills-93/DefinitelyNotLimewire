// tracker.cpp
// Tracker server for DefinitelyNotLimewire
// PLACEHOLDER for now (only reads the config file and prints it)
// Real server (sockets, worker threads, LIST / GET / 
// createtracker / updatetracker) will be written here

#include <iostream>
#include "common.h"

int main() {
    // sconfig: line 1 = port number, line 2 = shared folder (torrents)
    std::vector<std::string> config = readConfigLines("sconfig");

    // Stop with an error if the file is missing/incomplete
    if (config.size() < 2) {
        std::cerr << "Tracker: could not read sconfig (need port and folder)\n";
        return 1;
    }

    std::cout << "Tracker: port " << config[0]
              << ", shared folder " << config[1] << "\n";
    std::cout << "Tracker: not implemented yet\n";
    return 0;
}
