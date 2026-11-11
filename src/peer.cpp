// peer.cpp
// Peer program for DefinitelyNotLimewire
// PLACEHOLDER: for now this only reads the two config files and prints them
// The real peer (talking to the tracker, downloading chunks from other peers with threads, 
// serving chunks) will be written here.

#include <iostream>
#include "common.h"

int main() {
    // clientThreadConfig.cfg: tracker port, tracker IP, update interval (seconds)
    std::vector<std::string> client = readConfigLines("clientThreadConfig.cfg");

    // serverThreadConfig.cfg: this peer's listening port, shared folder name
    std::vector<std::string> server = readConfigLines("serverThreadConfig.cfg");

    // Stop with an error if either file is missing/incomplete
    if (client.size() < 3 || server.size() < 2) {
        std::cerr << "Peer: could not read config files\n";
        return 1;
    }

    std::cout << "Peer: tracker at " << client[1] << ":" << client[0]
              << ", update every " << client.back() << "s\n";
    std::cout << "Peer: listening port " << server[0]
              << ", shared folder " << server.back() << "\n";
    std::cout << "Peer: not implemented yet\n";
    return 0;
}