// test_tracker_file.cpp
// Standalone test for the TrackerFile class (not part of the build).
// Compile and run it from the project root, e.g.:
//   g++ -std=c++17 test_tracker_file.cpp src/tracker_file.cpp -o test_tracker_file
//   ./test_tracker_file && rm test_tracker_file
// Each check prints PASS or FAIL; the exit code is 0 only if all passed.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "src/tracker_file.h"

namespace {

int failedChecks = 0;

void check(bool ok, const std::string& what) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << what << "\n";
    if (!ok) {
        failedChecks++;
    }
}

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

// The tracker files this test creates (removed at start and at exit).
const char* testFiles[] = {"spec_example.track", "comments.track", "roundtrip.track",
                           "malformed.track"};

void cleanup() {
    for (const char* file : testFiles) {
        std::remove(file);
        std::remove((std::string(file) + ".tmp").c_str());
    }
}

}  // namespace

int main() {
    cleanup();  // in case an earlier run was killed half-way

    // --- the spec example parses ---
    writeFile("spec_example.track",
              "Filename: movie1.avi\n"
              "Filesize: 1000000\n"
              "Description: A short movie\n"
              "MD5: 524afd5e5b236a29d54acf5c441433e8\n"
              "#list of peers follows next\n"
              "128.83.4.15:6786:0:1000:1498117000\n"
              "128.83.4.16:6786:1001:2000:1498117100\n");

    TrackerFile spec;
    check(spec.load("spec_example.track"), "the spec example loads");
    check(spec.filename == "movie1.avi" && spec.filesize == 1000000 &&
              spec.description == "A short movie" &&
              spec.md5 == "524afd5e5b236a29d54acf5c441433e8",
          "spec example: header fields");
    check(spec.peers.size() == 2 &&
              spec.peers[0].ip == "128.83.4.15" && spec.peers[0].port == 6786 &&
              spec.peers[0].startByte == 0 && spec.peers[0].endByte == 1000 &&
              spec.peers[0].timestamp == 1498117000 &&
              spec.peers[1].ip == "128.83.4.16" && spec.peers[1].port == 6786 &&
              spec.peers[1].startByte == 1001 && spec.peers[1].endByte == 2000 &&
              spec.peers[1].timestamp == 1498117100,
          "spec example: two peers with the right fields");

    // --- comments and blank lines are ignored ---
    writeFile("comments.track",
              "\n"
              "# a comment line\n"
              "Filename: movie2.avi\n"
              "   \n"
              "#another comment\n"
              "Filesize: 500\n"
              "Description: spaced out\n"
              "\n"
              "MD5: 0123456789abcdef0123456789abcdef\n"
              "#list of peers follows next\n"
              "128.83.4.20:6787:0:499:1498117200\n"
              "# trailing comment\n");

    TrackerFile comments;
    check(comments.load("comments.track") && comments.filename == "movie2.avi" &&
              comments.filesize == 500 && comments.description == "spaced out" &&
              comments.md5 == "0123456789abcdef0123456789abcdef" &&
              comments.peers.size() == 1 && comments.peers[0].ip == "128.83.4.20",
          "comments and blank lines are ignored");

    // --- save() then load() gives the same data ---
    std::time_t beforeSave = std::time(nullptr);
    TrackerFile fresh = TrackerFile::createSeed("notes.txt", 4096,
                                                "00112233445566778899aabbccddeeff",
                                                "class notes", "127.0.0.1", 9001);
    fresh.upsertPeer("127.0.0.2", 9002, 0, 1023);
    check(fresh.peers.size() == 2 && fresh.peers[0].startByte == 0 &&
              fresh.peers[0].endByte == 4096 && fresh.peers[0].timestamp >= beforeSave,
          "createSeed() seeds 0..filesize with the current time");

    check(fresh.save("roundtrip.track"), "save() writes the file");
    TrackerFile loaded;
    check(loaded.load("roundtrip.track"), "the saved file loads back");
    check(loaded.filename == fresh.filename && loaded.filesize == fresh.filesize &&
              loaded.description == fresh.description && loaded.md5 == fresh.md5 &&
              loaded.peers.size() == 2 &&
              loaded.peers[0].ip == "127.0.0.1" && loaded.peers[0].port == 9001 &&
              loaded.peers[0].startByte == 0 && loaded.peers[0].endByte == 4096 &&
              loaded.peers[0].timestamp == fresh.peers[0].timestamp &&
              loaded.peers[1].ip == "127.0.0.2" && loaded.peers[1].port == 9002 &&
              loaded.peers[1].startByte == 0 && loaded.peers[1].endByte == 1023 &&
              loaded.peers[1].timestamp == fresh.peers[1].timestamp,
          "save() then load() gives the same data");

    // --- upsert of an existing peer updates it instead of duplicating ---
    std::time_t beforeUpsert = std::time(nullptr);
    loaded.upsertPeer("127.0.0.1", 9001, 50, 99);
    check(loaded.peers.size() == 2, "upsert of an existing peer does not duplicate it");
    check(loaded.peers[0].startByte == 50 && loaded.peers[0].endByte == 99,
          "upsert updates the byte range");
    check(loaded.peers[0].timestamp >= beforeUpsert, "upsert refreshes the time stamp");

    // --- pruneDead removes only expired peers ---
    std::time_t now = std::time(nullptr);
    TrackerFile pr;
    pr.filename = "pr.avi";
    pr.filesize = 10;
    pr.md5 = "bb";
    PeerEntry dead;
    dead.ip = "127.0.0.7";
    dead.port = 9200;
    dead.startByte = 0;
    dead.endByte = 9;
    dead.timestamp = now - 2000;  // too old
    PeerEntry alive;
    alive.ip = "127.0.0.8";
    alive.port = 9201;
    alive.startByte = 0;
    alive.endByte = 9;
    alive.timestamp = now - 10;  // fresh enough
    pr.peers.push_back(dead);
    pr.peers.push_back(alive);

    check(pr.pruneDead(1000) == 1, "pruneDead removes the expired peer");
    check(pr.peers.size() == 1 && pr.peers[0].ip == alive.ip && pr.peers[0].port == alive.port,
          "pruneDead keeps the fresh peer");

    // --- a malformed peer line makes load() return false ---
    writeFile("malformed.track",
              "Filename: m.avi\nFilesize: 10\nMD5: cc\n127.0.0.1:9000:0:9\n");
    TrackerFile bad;
    check(!bad.load("malformed.track"), "a malformed peer line fails load()");

    // the other load() failure cases from the class spec
    check(!bad.load("no_such_file.track"), "a missing file fails load()");
    writeFile("malformed.track",
              "Filename: m.avi\nFilesize: 10\n127.0.0.1:9000:0:9:123\n");
    check(!bad.load("malformed.track"), "a missing required field fails load()");

    cleanup();
    if (failedChecks == 0) {
        std::cout << "All checks passed\n";
        return 0;
    }
    std::cout << failedChecks << " check(s) failed\n";
    return 1;
}
