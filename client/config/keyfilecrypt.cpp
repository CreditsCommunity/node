#include "keyfilecrypt.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>

#include <base58.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cs {
namespace keyfile {

namespace {
using FileKey = cscrypto::MemGuard<cscrypto::Byte, crypto_aead_chacha20poly1305_KEYBYTES>;

// cscrypto's salt (cscrypto/src/privatekey.cpp, getKeyFromPassword)
constexpr std::array<cscrypto::Byte, crypto_pwhash_SALTBYTES> kSalt = {0x49, 0x44, 0x49, 0x20, 0x4e, 0x41, 0x48, 0x55,
                                                                       0x59, 0x20, 0x42, 0x4c, 0x59, 0x41, 0x54, 0x27};

// cscrypto's derivation with its identifiers pinned: Argon2id v1.3 and the argon2id SENSITIVE
// limits are the values of crypto_pwhash_ALG_DEFAULT and crypto_pwhash_OPSLIMIT/MEMLIMIT_SENSITIVE
// it uses. False when the key memory or the derivation's memory cannot be had.
bool deriveKey(FileKey& key, const char* passwd, std::size_t pswdLen) {
    return key && crypto_pwhash(key.data(), key.size(), passwd, pswdLen, kSalt.data(), crypto_pwhash_argon2id_OPSLIMIT_SENSITIVE,
                                crypto_pwhash_argon2id_MEMLIMIT_SENSITIVE, crypto_pwhash_ALG_ARGON2ID13) == 0;
}
}  // namespace

cscrypto::Bytes encryptWithNonce(const cscrypto::PrivateKey& key, const char* passwd, std::size_t pswdLen, const cscrypto::Byte* nonce) {
    cscrypto::Bytes result;
    if (!key || nonce == nullptr) {
        return result;
    }

    FileKey fileKey;
    if (!deriveKey(fileKey, passwd, pswdLen)) {
        return result;
    }

    cscrypto::Bytes out(kEncryptedSize);
    std::memcpy(out.data(), nonce, kNonceSize);
    auto sk = key.access();
    unsigned long long sealedLen = 0;
    if (crypto_aead_chacha20poly1305_encrypt(out.data() + kNonceSize, &sealedLen, sk.data(), sk.size(), nullptr, 0, nullptr, out.data(),
                                             fileKey.data()) != 0 ||
        sealedLen != kSealedSize) {
        sodium_memzero(out.data(), out.size());
        return result;
    }

    result.swap(out);
    return result;
}

cscrypto::Bytes encrypt(const cscrypto::PrivateKey& key, const char* passwd, std::size_t pswdLen) {
    std::array<cscrypto::Byte, kNonceSize> nonce;
    randombytes_buf(nonce.data(), nonce.size());
    return encryptWithNonce(key, passwd, pswdLen, nonce.data());
}

cscrypto::PrivateKey decrypt(const cscrypto::Bytes& bytes, const char* passwd, std::size_t pswdLen) {
    if (bytes.size() != kEncryptedSize) {
        return cscrypto::PrivateKey();
    }

    FileKey fileKey;
    if (!deriveKey(fileKey, passwd, pswdLen)) {
        return cscrypto::PrivateKey();
    }

    cscrypto::MemGuard<cscrypto::Byte, cscrypto::kPrivateKeySize> sk;
    if (!sk) {
        return cscrypto::PrivateKey();
    }
    // the output is exactly 64 bytes, which libsodium also zeroes on a bad tag
    unsigned long long skLen = 0;
    if (crypto_aead_chacha20poly1305_decrypt(sk.data(), &skLen, nullptr, bytes.data() + kNonceSize, kSealedSize, nullptr, 0, bytes.data(),
                                             fileKey.data()) != 0 ||
        skLen != cscrypto::kPrivateKeySize) {
        return cscrypto::PrivateKey();
    }
    return cscrypto::PrivateKey::readFromBytes(sk);
}

bool writeKeyFile(const std::string& name, const std::string& data) {
#ifndef _WIN32
    std::string target = name;
    if (char* resolved = ::realpath(name.c_str(), nullptr)) {
        target = resolved;
        ::free(resolved);
    }
    const std::string tmp = target + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return false;
    }
    bool ok = ::fchmod(fd, 0600) == 0;
    size_t done = 0;
    while (ok && done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            ok = false;
            break;
        }
        done += static_cast<size_t>(n);
    }
    ok = ok && ::fsync(fd) == 0;
    ok = (::close(fd) == 0) && ok;
    if (!ok || ::rename(tmp.c_str(), target.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    const auto slash = target.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string(".") : (slash == 0 ? std::string("/") : target.substr(0, slash));
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {  // best effort: the rename is done either way
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
#else
    const std::string tmp = name + ".tmp";
    HANDLE h = ::CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = true;
    size_t done = 0;
    while (ok && done < data.size()) {
        DWORD n = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 20));
        ok = ::WriteFile(h, data.data() + done, chunk, &n, nullptr) && n > 0;
        done += n;
    }
    ok = ok && ::FlushFileBuffers(h);
    ok = ::CloseHandle(h) && ok;
    if (!ok || !::MoveFileExA(tmp.c_str(), name.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
#endif
}


namespace {
// the whole file; false when it cannot be read
bool readAll(const std::string& name, std::string& out) {
    std::ifstream f(name, std::ios::binary);
    if (!f.is_open()) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}

// the first line, as the node reads its key files (std::getline), without a trailing \r or blanks
std::string firstLine(const std::string& text) {
    std::string line = text.substr(0, text.find('\n'));
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    return line;
}

void wipe(std::string& s) {
    if (!s.empty()) {
        sodium_memzero(&s[0], s.size());
    }
    s.clear();
}

// a new file NAME with DATA, mode 0600, flushed; false when NAME exists (exists is set) or on an error
bool createNew(const std::string& name, const std::string& data, bool& exists) {
    exists = false;
#ifndef _WIN32
    const int fd = ::open(name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        exists = errno == EEXIST;
        return false;
    }
    bool ok = ::fchmod(fd, 0600) == 0;
    size_t done = 0;
    while (ok && done < data.size()) {
        const ssize_t n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            ok = false;
            break;
        }
        done += static_cast<size_t>(n);
    }
    ok = ok && ::fsync(fd) == 0;
    ok = (::close(fd) == 0) && ok;
    if (!ok) {
        ::unlink(name.c_str());
    }
    return ok;
#else
    HANDLE h = ::CreateFileA(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        exists = err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS;
        return false;
    }
    bool ok = true;
    DWORD n = 0;
    ok = data.empty() || (::WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &n, nullptr) && n == data.size());
    ok = ok && ::FlushFileBuffers(h);
    ok = ::CloseHandle(h) && ok;
    if (!ok) {
        ::DeleteFileA(name.c_str());
    }
    return ok;
#endif
}
}  // namespace

ToPlainResult toPlain(const std::string& pathToPublic, const std::string& pathToPrivate, const Unlock& unlock, const std::string& stamp) {
    ToPlainResult result;

    std::string content;
    if (!readAll(pathToPrivate, content)) {
        result.status = ToPlain::NoPrivateFile;
        return result;
    }
    std::string sk58 = firstLine(content);
    cscrypto::Bytes bytes;
    const bool decoded = DecodeBase58(sk58, bytes);
    wipe(sk58);

    if (decoded && bytes.size() == cscrypto::kPrivateKeySize) {
        auto key = cscrypto::PrivateKey::readFromBytes(bytes);
        sodium_memzero(bytes.data(), bytes.size());
        wipe(content);
        if (!key) {
            result.status = ToPlain::BadPrivateFile;
            return result;
        }
        const auto pk = cscrypto::getMatchingPublic(key);
        result.address = EncodeBase58(pk.data(), pk.data() + pk.size());
        result.status = ToPlain::AlreadyPlain;
        return result;
    }
    if (!decoded || bytes.size() != kEncryptedSize) {
        result.status = ToPlain::BadPrivateFile;
        return result;
    }

    // before the password: a key without its public key file is refused anyway
    std::string publicText;
    if (!readAll(pathToPublic, publicText)) {
        result.status = ToPlain::NoPublicFile;
        return result;
    }
    const std::string publicOnFile = firstLine(publicText);

    const cscrypto::PrivateKey key = unlock(bytes);
    if (!key) {
        result.status = ToPlain::Cancelled;
        return result;
    }
    const auto pk = cscrypto::getMatchingPublic(key);
    result.address = EncodeBase58(pk.data(), pk.data() + pk.size());
    if (result.address != publicOnFile) {
        result.status = ToPlain::PublicMismatch;
        return result;
    }

    // the encrypted original first, under a name not taken yet
    const std::string base = pathToPrivate + ".encrypted-" + stamp;
    bool kept = false;
    for (int i = 1; i <= 100 && !kept; ++i) {
        const std::string name = i == 1 ? base : base + "-" + std::to_string(i);
        bool exists = false;
        if (createNew(name, content, exists)) {
            result.backup = name;
            kept = true;
        }
        else if (!exists) {
            break;
        }
    }
    if (!kept) {
        result.status = ToPlain::BackupFailed;
        return result;
    }

    auto sk = key.access();
    std::string plain = EncodeBase58(sk.data(), sk.data() + sk.size());
    const bool written = writeKeyFile(pathToPrivate, plain);
    wipe(plain);
    result.status = written ? ToPlain::Done : ToPlain::WriteFailed;
    return result;
}

std::string timestamp() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

}  // namespace keyfile
}  // namespace cs
