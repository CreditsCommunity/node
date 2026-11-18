#define TESTING

#include "clientconfigmock.hpp"
#include "gtest/gtest.h"

#include <array>
#include <filesystem>
#include <functional>
#include <vector>
#include <limits>
#include <set>

#include <csnode/blockchain.hpp>
#include <csnode/blockchain_serializer.hpp>
#include <csnode/configholder.hpp>
#include <consensus.hpp>
#include <cscrypto/cscrypto.hpp>
#include <csnode/conveyer.hpp>
#include <csnode/odatastream.hpp>

#include <csdb/amount_commission.hpp>
#include <csdb/currency.hpp>
#include <csdb/pool.hpp>
#include <csnode/nodecore.hpp>

TEST(BlockChain, block_service_info) {
    csdb::Pool block{};

    ASSERT_FALSE(BlockChain::isBootstrap(block));
    BlockChain::setBootstrap(block, false);
    ASSERT_FALSE(BlockChain::isBootstrap(block));
    BlockChain::setBootstrap(block, true);
    ASSERT_TRUE(BlockChain::isBootstrap(block));
    BlockChain::setBootstrap(block, false);
    ASSERT_FALSE(BlockChain::isBootstrap(block));

    block = csdb::Pool{};

    ASSERT_FALSE(BlockChain::isBootstrap(block));
    BlockChain::setBootstrap(block, true);
    ASSERT_TRUE(BlockChain::isBootstrap(block));
    BlockChain::setBootstrap(block, true);
    ASSERT_TRUE(BlockChain::isBootstrap(block));
    BlockChain::setBootstrap(block, false);
    ASSERT_FALSE(BlockChain::isBootstrap(block));
}

// mining and staking are toggled independently (special transaction order 37) and
// restored from quick-start caches through these accessors
TEST(BlockChain, MiningAndStakingSettersAreIndependent) {
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    BlockChain blockChain(genesis, start);

    blockChain.setMiningOn(true);
    blockChain.setStakingOn(false);
    ASSERT_TRUE(blockChain.getMiningOn());
    ASSERT_FALSE(blockChain.getStakingOn());

    blockChain.setMiningOn(false);
    blockChain.setStakingOn(true);
    ASSERT_FALSE(blockChain.getMiningOn());
    ASSERT_TRUE(blockChain.getStakingOn());
}

// an "uncertain" own block is replaced in place by the network's version only when testContentEqual() holds;
// rewards and fees are credited through the trusted mask, so a different mask must not count as equal content
TEST(BlockChain, ContentEqualRequiresSameTrustedMask) {
    csdb::Pool own;
    own.set_sequence(10);
    own.add_number_trusted(3);
    own.add_real_trusted(0);

    csdb::Pool canonical = own.clone();
    ASSERT_TRUE(BlockChain::testContentEqual(own, canonical));

    canonical.add_real_trusted(cs::Utils::maskToBits(cs::Bytes{ 0, 255, 1 }));
    ASSERT_FALSE(BlockChain::testContentEqual(own, canonical));
}

// The block rebuilt from this node's block and the writer's round package must be the writer's block,
// byte for byte (its hash is what the package's signers signed): mask, timestamp and the block reward
// come from the package, everything else, the bootstrap flag included, from this node's block
TEST(BlockChain, BlockFromPackageMetaIsTheWritersBlock) {
    const cs::PublicKey k1{1}, k2{2}, k3{3}, k4{4}, k5{5};
    auto base = [&](const cs::Bytes& mask, const std::string& timestamp, const std::string& reward) {
        csdb::Pool p;
        p.set_sequence(1433);
        p.set_previous_hash(csdb::PoolHash::calc_from_data(cs::Bytes{1, 2, 3}));
        p.add_number_trusted(static_cast<uint8_t>(mask.size()));
        p.add_real_trusted(cs::Utils::maskToBits(mask));
        p.set_confidants({k1, k2, k3, k4, k5});
        p.setRoundCost(csdb::Amount(0));
        BlockChain::setTimestamp(p, timestamp);
        BlockChain::setBootstrap(p, true);
        if (!reward.empty()) {
            p.add_user_field(BlockChain::kFieldBlockReward, reward);
        }
        p.add_number_confirmations(3);
        p.add_confirmation_mask(7);
        p.add_round_confirmations({cs::Signature{}, cs::Signature{}, cs::Signature{}});
        return p;
    };
    const cs::Bytes writerMask{0, 1, 255, 255, 2};
    csdb::Pool writer = base(writerMask, "1792732732167", "reward-of-three");
    csdb::Pool own = base(cs::Bytes{0, 1, 2, 3, 4}, "1792732732100", "reward-of-five");
    ASSERT_TRUE(writer.compose());

    cs::PoolMetaInfo meta;
    meta.sequenceNumber = 1433;
    meta.previousHash = writer.previous_hash();
    meta.realTrustedMask = writerMask;
    meta.timestamp = "1792732732167";
    meta.reward = "reward-of-three";

    csdb::Pool rebuilt = BlockChain::blockFromPackageMeta(own, meta);
    ASSERT_TRUE(rebuilt.compose());
    EXPECT_EQ(rebuilt.hash(), writer.hash());
    EXPECT_TRUE(BlockChain::isBootstrap(rebuilt));

    // without a reward on either side
    csdb::Pool writerNoReward = base(writerMask, "1792732732167", "");
    ASSERT_TRUE(writerNoReward.compose());
    meta.reward.clear();
    csdb::Pool rebuiltNoReward = BlockChain::blockFromPackageMeta(base(cs::Bytes{0, 1, 2, 3, 4}, "1", "reward"), meta);
    ASSERT_TRUE(rebuiltNoReward.compose());
    EXPECT_EQ(rebuiltNoReward.hash(), writerNoReward.hash());
}

// these members are saved into quick-start caches and read back on the --set-bc-top path,
// possibly before any consensus-settings change has set them
TEST(BlockChain, ConsensusSettingsHaveDefinedDefaults) {
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    BlockChain blockChain(genesis, start);

    ASSERT_TRUE(blockChain.getBlockReward() == csdb::Amount{ 0 });
    ASSERT_TRUE(blockChain.getMiningCoefficient() == csdb::Amount{ 0 });
    ASSERT_FALSE(blockChain.getMiningOn());
    ASSERT_FALSE(blockChain.getStakingOn());
    ASSERT_EQ(blockChain.getTimeMinStage1(), 500u);
}

// a pending order-37 change and the order-9 StartingDPOS must survive a quick-start checkpoint,
// and a checkpoint written before this was saved must still load
TEST(BlockChain, PendingConsensusSettingsSurviveCheckpoint) {
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    const auto dir = std::filesystem::temp_directory_path() / "cs_pending_settings_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::set<cs::PublicKey> confidants;
    {
        BlockChain saved(genesis, start);
        BlockChain::PendingConsensusSettings pending;
        pending.round = 400;
        pending.stakingOn = true;
        pending.miningOn = true;
        pending.blockReward = csdb::Amount{ 1 };
        saved.setPendingConsensusSettings(pending);
        saved.setStartingDPOS(450);

        cs::BlockChain_Serializer serializer;
        serializer.bind(saved, confidants);
        serializer.save(dir);
    }

    BlockChain loaded(genesis, start);
    cs::BlockChain_Serializer serializer;
    serializer.bind(loaded, confidants);
    serializer.load(dir);
    ASSERT_EQ(loaded.getPendingConsensusSettings().round, 400u);
    ASSERT_TRUE(loaded.getPendingConsensusSettings().miningOn);
    ASSERT_TRUE(loaded.getPendingConsensusSettings().blockReward == csdb::Amount{ 1 });
    ASSERT_EQ(loaded.getStartingDPOS(), 450u);

    // older checkpoints have no companion file: nothing pending, StartingDPOS untouched
    std::filesystem::remove(dir / "consensus_pending.dat");
    serializer.load(dir);
    ASSERT_EQ(loaded.getPendingConsensusSettings().round, std::numeric_limits<cs::Sequence>::max());
    ASSERT_EQ(loaded.getStartingDPOS(), 0u);

    std::filesystem::remove_all(dir);
}

namespace {
// a transaction carrying a special order: u16 order number (little endian) then its fields
csdb::Transaction specialOrderTransaction(int64_t id, uint16_t order, uint64_t value) {
    csdb::Transaction transaction{id, csdb::Address::from_public_key(cs::PublicKey{}), csdb::Address::from_public_key(cs::PublicKey{}),
                                  csdb::Currency{1}, csdb::Amount{0}, csdb::AmountCommission{0.}, csdb::AmountCommission{0.}, cs::Signature{}};
    std::string payload(reinterpret_cast<const char*>(&order), sizeof(order));
    payload.append(reinterpret_cast<const char*>(&value), sizeof(value));
    transaction.add_user_field(cs::trx_uf::sp::managing, payload);
    return transaction;
}

uint64_t orderValue(const std::string& payload) {
    uint64_t value = 0;
    std::copy(payload.begin() + sizeof(uint16_t), payload.end(), reinterpret_cast<char*>(&value));
    return value;
}
}  // namespace

TEST(BlockChain, SpecialOrdersSurviveCheckpoint) {
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    const auto dir = std::filesystem::temp_directory_path() / "cs_special_orders_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::set<cs::PublicKey> confidants;
    {
        BlockChain saved(genesis, start);
        csdb::Pool first;
        first.add_transaction(specialOrderTransaction(1, 26, 1000));  // MaxTransactionSize
        first.add_transaction(specialOrderTransaction(2, 35, 777));   // syncroChangeRound
        first.add_transaction(specialOrderTransaction(3, 37, 1));     // kept by the pending settings instead
        saved.recordSpecialOrders(first);
        csdb::Pool second;
        second.add_transaction(specialOrderTransaction(4, 26, 2000));  // a later order of the same kind wins
        saved.recordSpecialOrders(second);
        ASSERT_EQ(saved.getSpecialOrders().size(), 2u);

        cs::BlockChain_Serializer serializer;
        serializer.bind(saved, confidants);
        serializer.save(dir);
    }

    BlockChain loaded(genesis, start);
    cs::BlockChain_Serializer serializer;
    serializer.bind(loaded, confidants);
    serializer.load(dir);
    const auto& orders = loaded.getSpecialOrders();
    ASSERT_EQ(orders.size(), 2u);
    ASSERT_EQ(orderValue(orders.at(26)), 2000u);
    ASSERT_EQ(orderValue(orders.at(35)), 777u);

    // older checkpoints have no companion file: nothing to replay
    std::filesystem::remove(dir / "consensus_pending.dat");
    serializer.load(dir);
    ASSERT_TRUE(loaded.getSpecialOrders().empty());

    std::filesystem::remove_all(dir);
}

namespace {
void setEverySetting(BlockChain& chain) {
    chain.setBlockReward(csdb::Amount{ 3 });
    chain.setMiningCoefficient(csdb::Amount{ 2 });
    chain.setStakingOn(true);
    chain.setMiningOn(true);
    chain.setTimeMinStage1(700);
    BlockChain::PendingConsensusSettings pending;
    pending.round = 900;
    pending.stakingOn = true;
    pending.blockReward = csdb::Amount{ 5 };
    chain.setPendingConsensusSettings(pending);
    chain.setStartingDPOS(450);
    csdb::Pool orders;
    orders.add_transaction(specialOrderTransaction(1, 26, 1000));
    orders.add_transaction(specialOrderTransaction(2, 35, 777));
    chain.recordSpecialOrders(orders);
}
}  // namespace

// a checkpoint saves the hash of the consensus settings with the caches, and a quick start
// rejects the checkpoint when the loaded settings hash differently: every field must come back
// from blockchain.dat and consensus_pending.dat exactly
TEST(BlockChain, ConsensusSettingsDigestSurvivesCheckpoint) {
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    const auto dir = std::filesystem::temp_directory_path() / "cs_settings_digest_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    std::set<cs::PublicKey> confidants;
    std::array<uint8_t, 32> savedDigest{};
    {
        BlockChain saved(genesis, start);
        setEverySetting(saved);
        savedDigest = saved.consensusSettingsDigest();
        ASSERT_EQ(saved.checkpointDigest().settings, savedDigest);
        cs::BlockChain_Serializer serializer;
        serializer.bind(saved, confidants);
        serializer.save(dir);
    }

    BlockChain loaded(genesis, start);
    ASSERT_NE(loaded.consensusSettingsDigest(), savedDigest);
    cs::BlockChain_Serializer serializer;
    serializer.bind(loaded, confidants);
    serializer.load(dir);
    ASSERT_EQ(loaded.consensusSettingsDigest(), savedDigest);

    std::filesystem::remove_all(dir);
}

TEST(BlockChain, ConsensusSettingsDigestCoversEverySetting) {
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    BlockChain base(genesis, start);
    setEverySetting(base);
    const auto digest = base.consensusSettingsDigest();

    const std::vector<std::function<void(BlockChain&)>> changes = {
        [](BlockChain& c) { c.setBlockReward(csdb::Amount{ 4 }); },
        [](BlockChain& c) { c.setMiningCoefficient(csdb::Amount{ 1 }); },
        [](BlockChain& c) { c.setStakingOn(false); },
        [](BlockChain& c) { c.setMiningOn(false); },
        [](BlockChain& c) { c.setTimeMinStage1(800); },
        [](BlockChain& c) { auto p = c.getPendingConsensusSettings(); p.round = 901; c.setPendingConsensusSettings(p); },
        [](BlockChain& c) { auto p = c.getPendingConsensusSettings(); p.miningCoefficient = csdb::Amount{ 9 }; c.setPendingConsensusSettings(p); },
        [](BlockChain& c) { c.setStartingDPOS(451); },
        [](BlockChain& c) { csdb::Pool o; o.add_transaction(specialOrderTransaction(3, 26, 2000)); c.recordSpecialOrders(o); },
    };
    for (size_t i = 0; i < changes.size(); ++i) {
        BlockChain changed(genesis, start);
        setEverySetting(changed);
        ASSERT_EQ(changed.consensusSettingsDigest(), digest);
        changes[i](changed);
        EXPECT_NE(changed.consensusSettingsDigest(), digest) << "change " << i;
    }
}

namespace {
// a clean default config so the guard reads the default bound (1000) and no deep-rollback override,
// independent of any config a prior test in the binary may have set.
void installDefaultConfig() {
    cs::ConfigHolder::instance().setConfig(Config{});
}
}  // namespace

// The longest-chain guard bounds an automatic rollback at Consensus::MaxAutoRollbackDepth below the
// head captured when the rollback episode began, and that reference is fixed for the episode.
TEST(BlockChain, LongestChainGuardBoundsRollbackDepth) {
    installDefaultConfig();
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    BlockChain chain(genesis, start);
    chain.test_resetRollbackEpisode();

    const cs::Sequence head = 5000;
    const uint64_t depth = Consensus::MaxAutoRollbackDepth;  // default 1000
    chain.test_setLastSequence(head);

    // the first call captures the high-water reference at the current head
    ASSERT_TRUE(chain.guardAutoRollback());
    EXPECT_TRUE(chain.test_rollbackEpisode());
    EXPECT_EQ(chain.test_rollbackHighWater(), head);

    // unwinding step by step is allowed right up to the floor (head - depth), and the reference does
    // not move as the head drops, so forward progress cannot reset it
    chain.test_setLastSequence(head - depth + 1);
    EXPECT_TRUE(chain.guardAutoRollback());
    EXPECT_EQ(chain.test_rollbackHighWater(), head);

    // the removal that would take the head to or below the floor is refused
    chain.test_setLastSequence(head - depth);
    EXPECT_FALSE(chain.guardAutoRollback());

    // refusing sets no permanent state: a fresh episode from a new head works again
    chain.test_resetRollbackEpisode();
    chain.test_setLastSequence(head - depth);
    EXPECT_TRUE(chain.guardAutoRollback());
    EXPECT_EQ(chain.test_rollbackHighWater(), head - depth);
}

// Near genesis the bound never underflows: a head below the depth can always be unwound to genesis.
TEST(BlockChain, LongestChainGuardNoUnderflowNearGenesis) {
    installDefaultConfig();
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    BlockChain chain(genesis, start);
    chain.test_resetRollbackEpisode();

    chain.test_setLastSequence(10);
    for (cs::Sequence s = 10; s > 0; --s) {
        chain.test_setLastSequence(s);
        EXPECT_TRUE(chain.guardAutoRollback()) << "seq " << s;
    }
}

// A node that adopted a community bootstrap table may unwind back to the sequence where it took the
// table, even past the ordinary depth bound; below that point the bound applies again.
TEST(BlockChain, LongestChainGuardCommunityBootstrapFloor) {
    installDefaultConfig();
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    BlockChain chain(genesis, start);
    chain.test_resetRollbackEpisode();

    const cs::Sequence head = 5000;
    const cs::Sequence tableSeq = 3500;  // deeper than head - 1000 = 4000
    chain.test_setLastSequence(head);
    chain.setCommunityBootstrapFloor(tableSeq);
    ASSERT_TRUE(chain.guardAutoRollback());  // captures high-water 5000

    // past the ordinary floor (4000) but still above the table sequence: allowed
    chain.test_setLastSequence(4000);
    EXPECT_TRUE(chain.guardAutoRollback());
    chain.test_setLastSequence(tableSeq + 1);
    EXPECT_TRUE(chain.guardAutoRollback());

    // at or below the table sequence the ordinary bound takes over again and refuses
    chain.test_setLastSequence(tableSeq);
    EXPECT_FALSE(chain.guardAutoRollback());
}

namespace {
struct GuardKeys {
    std::vector<cs::PublicKey> pub;
    std::vector<cs::PrivateKey> priv;
};

GuardKeys guardKeys(size_t n) {
    EXPECT_TRUE(cscrypto::cryptoInit());
    GuardKeys k;
    for (size_t i = 0; i < n; ++i) {
        cs::PublicKey p;
        k.priv.push_back(cscrypto::generateKeyPair(p));
        k.pub.push_back(p);
    }
    return k;
}

// a block of `seq` whose confidants are `keys`, all of them signing; when `prev` is given it carries
// round confirmations by the confidants of `prev` selected in `confirmMask` (bit i = confidant i)
csdb::Pool guardBlock(cs::Sequence seq, const GuardKeys& keys, const GuardKeys* prev = nullptr,
                      uint64_t confirmMask = 0, bool bootstrap = false, bool corruptSignature = false) {
    csdb::Pool p(csdb::PoolHash{}, seq);
    p.set_confidants(keys.pub);
    p.add_number_trusted(static_cast<uint8_t>(keys.pub.size()));
    p.add_real_trusted((1ull << keys.pub.size()) - 1);
    p.add_user_field(200, std::string("payload"));  // an id no block field uses
    BlockChain::setBootstrap(p, bootstrap);
    if (prev) {
        cs::Bytes trustedToHash;
        cs::ODataStream stream(trustedToHash);
        stream << seq;
        stream << keys.pub;
        const cs::Hash h = cscrypto::calculateHash(trustedToHash.data(), trustedToHash.size());
        std::vector<cs::Signature> confirmations;
        for (size_t i = 0; i < prev->priv.size(); ++i) {
            if (confirmMask >> i & 1) {
                confirmations.push_back(cscrypto::generateSignature(prev->priv[i], h.data(), h.size()));
            }
        }
        p.add_number_confirmations(static_cast<uint8_t>(prev->pub.size()));
        p.add_confirmation_mask(confirmMask);
        p.add_round_confirmations(confirmations);
    }
    auto probe = p.clone();
    EXPECT_TRUE(probe.compose());
    cs::Hash hash{};
    const auto hb = probe.hash().to_binary();
    std::copy(hb.begin(), hb.end(), hash.begin());
    std::vector<cs::Signature> sigs;
    for (const auto& k : keys.priv) {
        sigs.push_back(cscrypto::generateSignature(k, hash.data(), hash.size()));
    }
    if (corruptSignature) {
        sigs.front()[0] ^= 0xff;
    }
    p.set_signatures(sigs);
    EXPECT_TRUE(p.compose());
    return csdb::Pool::from_binary(p.to_binary());
}

BlockChain& guardChain() {
    static const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    static const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    static BlockChain chain(genesis, start);
    return chain;
}
}  // namespace

// A non-bootstrap competing block may make the node unwind only with confirmations by a majority of
// the confidants of the node's own last block; stricter than block validation (no one-entry masks).
TEST(BlockChain, CompetingBlockNeedsStrictTrustedConfirmations) {
    installDefaultConfig();
    auto& chain = guardChain();
    const auto ours = guardKeys(3);
    const auto next = guardKeys(3);
    chain.test_setLastBlock(guardBlock(20, ours));

    EXPECT_TRUE(chain.hasTrustedConfirmations(guardBlock(21, next, &ours, 0b111)));
    EXPECT_TRUE(chain.hasTrustedConfirmations(guardBlock(21, next, &ours, 0b011)));   // 2 of 3
    EXPECT_FALSE(chain.hasTrustedConfirmations(guardBlock(21, next, &ours, 0b001)));  // a minority
    EXPECT_FALSE(chain.hasTrustedConfirmations(guardBlock(21, next)));                // none
    const auto strangers = guardKeys(3);
    EXPECT_FALSE(chain.hasTrustedConfirmations(guardBlock(21, next, &strangers, 0b111)));  // keys we do not trust
    const auto four = guardKeys(4);
    EXPECT_FALSE(chain.hasTrustedConfirmations(guardBlock(21, next, &four, 0b1111)));      // mask of another size
    EXPECT_TRUE(chain.competingBlockMayTriggerUnwind(guardBlock(21, next, &ours, 0b111)));
    EXPECT_FALSE(chain.competingBlockMayTriggerUnwind(guardBlock(21, next, &strangers, 0b111)));
}

// The bootstrap flag is a bit anyone can set: a bootstrap-flagged competing block passes the gate
// only when it is authenticated locally (valid own signatures AND the confidants of a round table this
// node accepted), never on the flag alone, and never more easily than a normal block.
TEST(BlockChain, BootstrapFlagAloneNeverTriggersUnwind) {
    installDefaultConfig();
    auto& chain = guardChain();
    const auto ours = guardKeys(3);
    const auto others = guardKeys(3);
    chain.test_setLastBlock(guardBlock(30, ours));

    auto& conveyer = cs::Conveyer::instance();
    cs::RoundTable unrelated;
    unrelated.round = 1000;
    unrelated.confidants = guardKeys(3).pub;
    conveyer.setRound(unrelated.round);
    conveyer.setTable(unrelated);

    const auto flagged = guardBlock(31, others, nullptr, 0, true);
    ASSERT_TRUE(BlockChain::isBootstrap(flagged));
    EXPECT_FALSE(chain.isBootstrapAuthenticatedLocally(flagged));
    EXPECT_FALSE(chain.competingBlockMayTriggerUnwind(flagged));
    // the same block without the flag fails too: the flag never opens a gate a normal block cannot pass
    EXPECT_FALSE(chain.competingBlockMayTriggerUnwind(guardBlock(31, others)));

    // the node took a round table naming exactly these confidants: the bootstrap block is authenticated
    cs::RoundTable taken;
    taken.round = 1001;
    taken.confidants = others.pub;
    conveyer.setRound(taken.round);
    conveyer.setTable(taken);
    EXPECT_TRUE(chain.isBootstrapAuthenticatedLocally(flagged));
    EXPECT_TRUE(chain.competingBlockMayTriggerUnwind(flagged));
    // ... but not with a bad signature
    EXPECT_FALSE(chain.competingBlockMayTriggerUnwind(guardBlock(31, others, nullptr, 0, true, true)));

    // leave the conveyer on a table that matches nothing here
    unrelated.round = 1002;
    conveyer.setRound(unrelated.round);
    conveyer.setTable(unrelated);
}
