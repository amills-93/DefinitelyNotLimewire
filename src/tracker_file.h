// tracker_file.h
// TrackerFile: parses, edits and writes the .track tracker files that
// the tracker server stores in its shared folder (see the project spec)

#ifndef TRACKER_FILE_H
#define TRACKER_FILE_H

#include <cstddef>
#include <ctime>
#include <string>
#include <vector>

// One peer line of a tracker file: <ip>:<port>:<start>:<end>:<time stamp>
struct PeerEntry {
    std::string ip;
    int port = 0;
    long long startByte = 0;
    long long endByte = 0;
    std::time_t timestamp = 0;  // Unix time in seconds
};

// One tracker file: the header fields plus one entry per peer.
//
// Not thread-safe: callers must hold one mutex around each
// load -> modify -> save sequence. The class adds no locking itself.
class TrackerFile {
public:
    // Header fields (Description is optional and may be empty)
    std::string filename;
    long long filesize = 0;
    std::string description;
    std::string md5;               // MD5 of the shared file
    std::vector<PeerEntry> peers;  // peers that currently hold the file

    /// Parses the tracker file at path. Blank lines and #-comments are
    /// skipped. Returns false (and leaves this object untouched) if the
    /// file is missing, a required field (Filename/Filesize/MD5) is
    /// missing, or any peer line is malformed.
    bool load(const std::string& path);

    /// Writes the tracker to path, exactly as toString() shows it. The
    /// data goes to path + ".tmp" first and is then renamed over path, so
    /// a reader never sees a half-written file. Returns false on I/O errors.
    bool save(const std::string& path) const;

    /// Adds the peer, or updates the existing peer with the same ip and
    /// port: its byte range is replaced and its time stamp refreshed.
    /// New peers are stamped with the current time.
    void upsertPeer(const std::string& ip, int port,
                    long long startByte, long long endByte);

    /// Removes peers whose time stamp is older than maxAgeSeconds and
    /// returns how many were removed. The caller reads the interval
    /// from config; nothing is hardcoded here.
    std::size_t pruneDead(long long maxAgeSeconds);

    /// Returns the full tracker file content, exactly what save() writes.
    /// The GET reply wraps this between <REP GET BEGIN> and
    /// <REP GET END FileMD5>; the caller computes that MD5.
    std::string toString() const;

    /// Returns the peers sorted by time stamp, newest first. The peer
    /// side uses this to pick the peer to download a segment from.
    std::vector<PeerEntry> peersByNewest() const;

    /// Builds a brand-new tracker for a createtracker message: the header
    /// fields come from the message, and the seed peer (start 0, end
    /// filesize, current time) is the first entry.
    static TrackerFile createSeed(const std::string& filename, long long filesize,
                                  const std::string& md5, const std::string& description,
                                  const std::string& seedIp, int seedPort);
};

#endif
