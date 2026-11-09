// What a confidant does with the stage 1 it already signed when it enters stage 1 again in the
// same round and sub-round (cs::stageOneResend), and that the message it then sends again verifies
// at a receiver exactly as the first copy did (the check of Node::getStageOne).

#include <stage.hpp>

#include <consensus.hpp>
#include <cscrypto/cscrypto.hpp>
#include <csnode/datastream.hpp>

#include <string>

#include "gtest/gtest.h"

namespace {

cs::Hash hashOf(uint8_t b) {
    cs::Hash h;
    h.fill(b);
    return h;
}

cs::PublicKey keyOf(uint8_t b) {
    cs::PublicKey k;
    k.fill(b);
    return k;
}

cs::StageOne stageOne(uint64_t stamp) {
    cs::StageOne st;
    st.sender = 2;
    st.hash = hashOf(7);
    st.trustedCandidates = {keyOf(1), keyOf(2), keyOf(3)};
    st.roundTimeStamp = std::to_string(stamp);
    return st;
}

// the bytes a receiver checks the signature against: round, sub-round, hash of the message
cs::Bytes signedPart(cs::RoundNumber round, uint8_t subRound, const cs::Bytes& message) {
    const cs::Hash messageHash = cscrypto::calculateHash(message.data(), message.size());
    cs::Bytes out;
    cs::ODataStream stream(out);
    stream << round;
    stream << subRound;
    stream << messageHash;
    return out;
}

constexpr uint64_t kLast = 1'700'000'000'000ULL;  // last block's stamp

}  // namespace

TEST(StageOneResend, ResendsWhenOnlyTheVotesDiffer) {
    const cs::StageOne sent = stageOne(kLast + 500);
    EXPECT_EQ(cs::stageOneResend(sent, sent.hash, kLast, kLast + 1500), cs::StageOneResend::Resend);
}

TEST(StageOneResend, SignsAgainWhenTheCharacteristicHashChanged) {
    const cs::StageOne sent = stageOne(kLast + 500);
    EXPECT_EQ(cs::stageOneResend(sent, hashOf(8), kLast, kLast + 1500), cs::StageOneResend::HashChanged);
}

TEST(StageOneResend, SignsAgainWhenTheStampIsNotAfterTheLastBlock) {
    const cs::StageOne sent = stageOne(kLast + 500);
    // the replacement package carried a later last block
    EXPECT_EQ(cs::stageOneResend(sent, sent.hash, kLast + 500, kLast + 1500), cs::StageOneResend::NotAfterLastBlock);
    EXPECT_EQ(cs::stageOneResend(sent, sent.hash, kLast + 900, kLast + 1500), cs::StageOneResend::NotAfterLastBlock);
    EXPECT_EQ(cs::stageOneResend(sent, sent.hash, kLast + 499, kLast + 1500), cs::StageOneResend::Resend);
}

TEST(StageOneResend, SignsAgainWhenTheStampIsUnreadable) {
    cs::StageOne sent = stageOne(kLast + 500);
    sent.roundTimeStamp = "";
    EXPECT_EQ(cs::stageOneResend(sent, sent.hash, kLast, kLast + 1500), cs::StageOneResend::Unreadable);
    sent.roundTimeStamp = "x";
    EXPECT_EQ(cs::stageOneResend(sent, sent.hash, kLast, kLast + 1500), cs::StageOneResend::Unreadable);
}

TEST(StageOneResend, SignsAgainWhenTheStampIsTooOld) {
    const uint64_t stamp = kLast + 500;
    const cs::StageOne sent = stageOne(stamp);
    EXPECT_EQ(cs::stageOneResend(sent, sent.hash, kLast, stamp + Consensus::DefaultTimeStampRange), cs::StageOneResend::Resend);
    EXPECT_EQ(cs::stageOneResend(sent, sent.hash, kLast, stamp + Consensus::DefaultTimeStampRange + 1), cs::StageOneResend::TooOld);
    // a clock behind the stored stamp is not "too old"
    EXPECT_EQ(cs::stageOneResend(sent, sent.hash, kLast, stamp - 100), cs::StageOneResend::Resend);
}

TEST(StageOneResend, HashCheckComesFirst) {
    cs::StageOne sent = stageOne(kLast);  // not after the last block either
    EXPECT_EQ(cs::stageOneResend(sent, hashOf(9), kLast, kLast + 1), cs::StageOneResend::HashChanged);
}

TEST(StageOneResend, ContentDiff) {
    const cs::StageOne a = stageOne(kLast + 1);
    cs::StageOne b = stageOne(kLast + 2);  // only the stamp differs
    EXPECT_EQ(cs::stageOneContentDiff(a, b), "");

    // candidates compare as a set: the order is the arrival order
    b.trustedCandidates = {keyOf(3), keyOf(1), keyOf(2)};
    EXPECT_EQ(cs::stageOneContentDiff(a, b), "");

    b.trustedCandidates.push_back(keyOf(4));
    EXPECT_EQ(cs::stageOneContentDiff(a, b), "candidates");

    b.hashesCandidates.push_back(cs::TransactionsPacketHash{});
    EXPECT_EQ(cs::stageOneContentDiff(a, b), "candidates,packets");

    b.hash = hashOf(1);
    EXPECT_EQ(cs::stageOneContentDiff(a, b), "hash,candidates,packets");
}

TEST(StageOneResend, ReasonNames) {
    EXPECT_STREQ(cs::toString(cs::StageOneResend::Resend), "resend");
    EXPECT_STREQ(cs::toString(cs::StageOneResend::HashChanged), "hash-changed");
    EXPECT_STREQ(cs::toString(cs::StageOneResend::NotAfterLastBlock), "not-after-last-block");
    EXPECT_STREQ(cs::toString(cs::StageOneResend::TooOld), "too-old");
    EXPECT_STREQ(cs::toString(cs::StageOneResend::Unreadable), "unreadable");
}

TEST(StageOneResend, ResentBytesVerifyAsTheFirstCopy) {
    cs::PublicKey pub;
    const auto priv = cscrypto::generateKeyPair(pub);
    const cs::RoundNumber round = 1234;
    const uint8_t subRound = 0;

    // signed as TrustedStage1State::finalizeStage() does
    cs::StageOne first = stageOne(kLast + 500);
    first.toBytes();
    first.messageHash = cscrypto::calculateHash(first.message.data(), first.message.size());
    const cs::Bytes part = signedPart(round, subRound, first.message);
    first.signature = cscrypto::generateSignature(priv, part.data(), part.size());

    // the re-entered stage 1 resends the stored message unchanged
    ASSERT_EQ(cs::stageOneResend(first, first.hash, kLast, kLast + 1500), cs::StageOneResend::Resend);
    const cs::StageOne resent = first;
    EXPECT_EQ(resent.message, first.message);
    EXPECT_EQ(resent.signature, first.signature);

    // a receiver in the same round and sub-round verifies it (Node::getStageOne)
    const cs::Bytes atReceiver = signedPart(round, subRound, resent.message);
    EXPECT_TRUE(cscrypto::verifySignature(resent.signature, pub, atReceiver.data(), atReceiver.size()));
    // and refuses it in another round or sub-round, as for any stage 1
    const cs::Bytes otherRound = signedPart(round + 1, subRound, resent.message);
    EXPECT_FALSE(cscrypto::verifySignature(resent.signature, pub, otherRound.data(), otherRound.size()));
    const cs::Bytes otherSub = signedPart(round, subRound + 1, resent.message);
    EXPECT_FALSE(cscrypto::verifySignature(resent.signature, pub, otherSub.data(), otherSub.size()));

    // the stamp decoded from the resent message is the first one, so a receiver that already
    // holds the first copy ignores the resent one (SolverCore::gotStageOne keeps a later stamp only)
    cs::StageOne decoded;
    cs::IDataStream stream(resent.message.data(), resent.message.size());
    stream >> decoded.sender >> decoded.hash >> decoded.trustedCandidates >> decoded.hashesCandidates >> decoded.roundTimeStamp;
    ASSERT_TRUE(stream.isValid());
    EXPECT_EQ(decoded.roundTimeStamp, first.roundTimeStamp);
}
