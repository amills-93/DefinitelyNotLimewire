// md5.cpp
// MD5 checksum helpers (see md5.h), built on OpenSSL's EVP interface.

#include "md5.h"

#include <openssl/evp.h>

#include <cstdio>
#include <fstream>

namespace {

// Turns the 16 raw MD5 bytes into 32 hex characters, e.g. "c68c2ee8..."
std::string toHex(const unsigned char* bytes, unsigned int length) {
    std::string hex;
    char pair[3];
    for (unsigned int i = 0; i < length; i++) {
        std::snprintf(pair, sizeof(pair), "%02x", bytes[i]);
        hex += pair;
    }
    return hex;
}

}  

std::string md5OfString(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    // One call: hash all of data with MD5 into digest
    if (EVP_Digest(data.data(), data.size(), digest, &length, EVP_md5(), nullptr) != 1) {
        return "";
    }
    return toHex(digest, length);
}

std::string md5OfFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return "";  // missing or unreadable file
    }

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr || EVP_DigestInit_ex(context, EVP_md5(), nullptr) != 1) {
        EVP_MD_CTX_free(context);
        return "";
    }

    // Feed the file to MD5 in pieces
    char buffer[8192];
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        if (EVP_DigestUpdate(context, buffer, static_cast<std::size_t>(file.gcount())) != 1) {
            EVP_MD_CTX_free(context);
            return "";
        }
    }
    if (file.bad()) {  // a real read error, not just reaching the end
        EVP_MD_CTX_free(context);
        return "";
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    int ok = EVP_DigestFinal_ex(context, digest, &length);
    EVP_MD_CTX_free(context);
    if (ok != 1) {
        return "";
    }
    return toHex(digest, length);
}