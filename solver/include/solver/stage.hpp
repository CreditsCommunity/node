#ifndef STAGE_HPP
#define STAGE_HPP

#include <consensus.hpp>
#include <csnode/nodecore.hpp>
#include <cstdint>
#include <lib/system/utils.hpp>
#include <packet.hpp>

namespace cs {

constexpr const uint8_t InvalidSender = uint8_t(-1);

class TrustedMask {
public:
    TrustedMask();
    TrustedMask(uint8_t size);

    TrustedMask& operator=(const TrustedMask& mask) {
        init(mask.value());
        return *this;
    }

    void init(uint8_t size);
    void init(const std::vector<uint8_t>& newMask);
    std::string toString();
    static std::string toString(std::vector<uint8_t> mask);
    void setMask(uint8_t index, uint8_t value);
    uint8_t trustedSize();
    static uint8_t trustedSize(const std::vector<uint8_t>& mask);
    void toReadOnly() {
        isReadOnly_ = true;
    }

    bool isReadOnly() {
        return isReadOnly_;
    }

    uint8_t size() {
        return static_cast<uint8_t>(mask_.size());
    }

    const std::vector<uint8_t> value() const {
        return mask_;
    }

    uint8_t value(size_t index) const {
        if (index < mask_.size()) {
            return mask_[index];
        }
        return 255U;
    }

private:
    std::vector<uint8_t> mask_;
    bool isReadOnly_;
};

struct Stage {
    Stage();
    uint8_t msgType;
    uint8_t sender;
    cs::PublicKey senderKey;
    cs::Bytes message;
    cs::RoundNumber msgRoundNum;
    cs::PublicKey msgSender;
    cs::Signature signature;
};

struct StageOne : Stage {
    StageOne();
    void toBytes();
    static StageOne fromBytes(const cs::Bytes bytes);
    static std::string toString(const StageOne stage);
    Hash hash;
    std::string roundTimeStamp;
    std::vector<PublicKey> trustedCandidates;
    std::vector<TransactionsPacketHash> hashesCandidates;
    Hash messageHash;
    //TrustedMask stageMask;
};

struct StageTwo : Stage {
    StageTwo();
    void toBytes();
    static StageTwo fromBytes(const cs::Bytes bytes);
    static std::string toString(const StageTwo stage);
    cs::Hashes hashes;  // hashes of stage one
    cs::Signatures signatures;

    //TrustedMask stageMask;
};


struct StageThree : Stage {
    StageThree();
    void toBytes();
    static StageThree fromBytes(const cs::Bytes bytes);
    static std::string toString(const StageThree stage);
    uint8_t writer;
    uint8_t iteration;
    std::vector<uint8_t> realTrustedMask;
    Signature blockSignature;
    Hash blockHash;
    Signature roundSignature;
    Hash roundHash;
    Hash trustedHash;
    Signature trustedSignature;
};

// smart-contracts stages
struct StageOneSmarts {
    bool fillBinary();
    bool fillFromBinary();
    uint8_t sender;
    uint64_t id = 0;  // combination of starter params: block number, transaction number, counter
    // cs::Sequence sBlockNum;
    // uint32_t startTransaction;
    std::vector <csdb::Amount> fees;
    Hash hash;
    Hash messageHash;
    Signature signature;
    Bytes message;
};

struct StageTwoSmarts {
    Bytes toBinary();
    bool fromBinary(Bytes message, StageTwoSmarts& stage);
    uint8_t sender;
    uint64_t id;  // combination of starter params: block number, transaction number, counter
    // cs::Sequence sBlockNum;
    // uint32_t startTransaction;
    std::vector<Hash> hashes;  // hashes of stage one
    std::vector<Signature> signatures;
    Signature signature;
    Bytes message;
};

struct StageThreeSmarts {
    Bytes toBinary();
    static bool fromBinary(Bytes message, StageThreeSmarts& stage);
    uint8_t sender;
    uint8_t iteration;
    uint64_t id;  // combination of starter params: block number, transaction number, counter
    // cs::Sequence sBlockNum;
    // uint32_t startTransaction;
    uint8_t writer;
    std::vector<uint8_t> realTrustedMask;
    Signature packageSignature;
    Signature signature;
    Bytes message;
};

struct StageHash {
    csdb::PoolHash hash;
    cs::PublicKey sender;
    cs::Byte trustedSize;
    cs::Byte realTrustedSize;
    uint64_t timeStamp;
    cs::RoundNumber round;
};

// What a confidant does with its stage 1 when it enters stage 1 again in the same round and
// sub-round from the same seat (SolverCore::nextRound(true), after a fuller round package of the
// round replaced the first one) and finds the stage 1 it already signed and sent:
// - Resend: the stored signed message is sent again byte for byte, so every confidant holds the
//   same stage 1 of this seat;
// - otherwise a new stage 1 is signed as before, because the stored one would be refused or
//   would make this seat look faulty: HashChanged (the characteristic hash must match the
//   majority's), NotAfterLastBlock (the stamp must be after the last block's), TooOld (the stored
//   stamp is more than Consensus::DefaultTimeStampRange old, so fresh stamps of the others could
//   fall outside this node's own stage-2 window, which is measured from its own stamp),
//   Unreadable (the stored stamp does not parse as a number).
enum class StageOneResend : uint8_t {
    Resend,
    HashChanged,
    NotAfterLastBlock,
    TooOld,
    Unreadable
};

// sent: the stored signed stage 1; freshHash: the characteristic hash computed now;
// lastBlockStamp: the last block's timestamp; now: the current time (ms)
StageOneResend stageOneResend(const StageOne& sent, const Hash& freshHash, uint64_t lastBlockStamp, uint64_t now);

const char* toString(StageOneResend decision);

// the content fields of two stage-1 messages of one seat that differ, timestamp and signature
// aside, comma separated (empty: the same content); trusted candidates compare as sets
std::string stageOneContentDiff(const StageOne& a, const StageOne& b);

}  // namespace cs

#endif  // STAGE_HPP
