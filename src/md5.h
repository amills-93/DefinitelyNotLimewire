// md5.h
// Shared by trsacker and peer
// Used for: the file MD5 in createtracker, the FileMD5 in the
// GET reply, and the peer checking that a received tracker is intact.
// Uses the OpenSSL library: https://www.openssl.org/docs/

#ifndef MD5_H
#define MD5_H

#include <string>

// Returns the MD5 of the given text/bytes as 32 lowercase hex characters
std::string md5OfString(const std::string& data);

// Returns the MD5 of the file at path as 32 lowercase hex characters,
// or an empty string if the file can't be read
std::string md5OfFile(const std::string& path);

#endif