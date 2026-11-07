#ifndef CLIENT_CONFIG_KEYFILECRYPT_HPP
#define CLIENT_CONFIG_KEYFILECRYPT_HPP

#include <cstddef>
#include <functional>
#include <string>

#include <cscrypto/cscrypto.hpp>

namespace cs {
namespace keyfile {

// The encrypted private key file (NodePrivate.txt), decoded from base58: the format of cscrypto's
// PrivateKey::getEncrypted / readFromEncrypted, which every node writes and reads, unchanged:
//
//   nonce (8) || crypto_aead_chacha20poly1305 (original construction, no associated data) of the
//   64-byte secret key: ciphertext + tag (64 + 16) = 88 bytes
//
// with the key derived by crypto_pwhash (Argon2id v1.3, OPSLIMIT_SENSITIVE, MEMLIMIT_SENSITIVE)
// and cscrypto's salt. The functions here write and read exactly those bytes; they differ from
// cscrypto's only in their checks: a failed allocation or derivation is an error (an empty result)
// and any other length is refused before decryption. 64 bytes is a plain key.
constexpr std::size_t kNonceSize = crypto_aead_chacha20poly1305_NPUBBYTES;
constexpr std::size_t kSealedSize = cscrypto::kPrivateKeySize + crypto_aead_chacha20poly1305_ABYTES;
constexpr std::size_t kEncryptedSize = kNonceSize + kSealedSize;  // 88

// a new encryption with a random nonce; empty on failure (no key, or the derivation failed)
cscrypto::Bytes encrypt(const cscrypto::PrivateKey& key, const char* passwd, std::size_t pswdLen);

// the same with the given nonce (kNonceSize bytes): for the tests, which compare the bytes with
// files written by cscrypto
cscrypto::Bytes encryptWithNonce(const cscrypto::PrivateKey& key, const char* passwd, std::size_t pswdLen, const cscrypto::Byte* nonce);

// an empty key on a wrong password, a changed file, another length or a failed derivation
cscrypto::PrivateKey decrypt(const cscrypto::Bytes& bytes, const char* passwd, std::size_t pswdLen);

// The private key file and the keys JSON: readable by their owner only (mode 0600 on POSIX whatever
// the umask; Windows keeps the folder's ACL), and replaced atomically: the data goes to NAME.tmp, is
// flushed to disk, then renamed over NAME (a link named NAME is followed, its target replaced), and
// the folder is flushed where the system allows it. On any failure NAME is left as it was and false
// is returned.
bool writeKeyFile(const std::string& name, const std::string& data);

// --decryptkey: the private key file encrypted by the node (88 bytes) becomes the plain key file (64
// bytes in base58, the format the node and keygen write). The encrypted file is kept first as
// <private>.encrypted-<stamp> (mode 0600, created new: an existing file of that name is never
// replaced, -2, -3... are added), then the plain key replaces the file with writeKeyFile. The public
// key file must exist and hold the key's public key, or nothing is changed.
enum class ToPlain {
    Done,            // written; address and backup are set
    AlreadyPlain,    // nothing to do; address is set
    NoPrivateFile,
    BadPrivateFile,  // neither a plain nor an encrypted key
    NoPublicFile,
    Cancelled,       // unlock returned no key (the input ended)
    PublicMismatch,  // address is the key's, NodePublic.txt holds another one
    BackupFailed,    // nothing changed
    WriteFailed      // the backup exists, the key file is as it was
};

struct ToPlainResult {
    ToPlain status = ToPlain::Cancelled;
    std::string address;  // base58 public key of the private key
    std::string backup;   // the copy of the encrypted file
};

// unlock gets the encrypted bytes and returns the key, or an empty key to give up; stamp is the
// backup's suffix (timestamp())
using Unlock = std::function<cscrypto::PrivateKey(const cscrypto::Bytes& encrypted)>;
ToPlainResult toPlain(const std::string& pathToPublic, const std::string& pathToPrivate, const Unlock& unlock, const std::string& stamp);

// the local time as YYYYMMDD-HHMMSS
std::string timestamp();

}  // namespace keyfile
}  // namespace cs

#endif  // CLIENT_CONFIG_KEYFILECRYPT_HPP
