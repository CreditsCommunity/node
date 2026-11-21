#define TESTING

#include "clientconfigmock.hpp"
#include "gtest/gtest.h"

#include <array>
#include <ctime>
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
#include <csnode/forkguardstate.hpp>
#include <config.hpp>

#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <fstream>
#include <sstream>
#include <chrono>
#include <cstdlib>

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
// a clean default config so the guard reads the default bound and no deep-rollback override,
// independent of any config a prior test in the binary may have set.
void installDefaultConfig() {
    cs::ConfigHolder::instance().setConfig(Config{});
}

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);  // an empty value removes the variable on Windows
#else
    ::setenv(name, value, 1);
#endif
}

void unsetEnv(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

// an empty folder of its own under the system temporary folder, removed with the object
struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const std::string& name) {
        path = std::filesystem::temp_directory_path() / ("csnode-guard-" + name + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string str() const { return path.string(); }
};
}  // namespace

static_assert(DEFAULT_MAX_AUTO_ROLLBACK == Consensus::MaxAutoRollbackDepth,
              "the config default and the consensus constant of the rollback bound must stay in step");
static_assert(Consensus::MaxAutoRollbackDepth == 100, "owner decision: default max_auto_rollback is 100");

// The longest-chain guard bounds an automatic rollback at Consensus::MaxAutoRollbackDepth below the
// head captured when the rollback episode began, and that reference is fixed for the episode.
TEST(BlockChain, LongestChainGuardBoundsRollbackDepth) {
    installDefaultConfig();
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    BlockChain chain(genesis, start);
    chain.test_resetRollbackEpisode();

    const cs::Sequence head = 5000;
    const uint64_t depth = Consensus::MaxAutoRollbackDepth;
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
    EXPECT_FALSE(chain.forkHeld());

    // the removal that would take the head to or below the floor is refused: FORK HELD
    chain.test_setLastSequence(head - depth);
    EXPECT_FALSE(chain.guardAutoRollback());
    EXPECT_TRUE(chain.forkHeld());

    // refusing sets no permanent state: a fresh episode from a new head works again
    chain.test_resetRollbackEpisode();
    chain.test_setLastSequence(head - depth);
    EXPECT_TRUE(chain.guardAutoRollback());
    EXPECT_EQ(chain.test_rollbackHighWater(), head - depth);
    EXPECT_FALSE(chain.forkHeld());
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

// [params] max_auto_rollback and allow_deep_rollback, and the CS_ALLOW_DEEP_ROLLBACK variable
TEST(BlockChain, RollbackGuardConfigParsing) {
    const auto parse = [](const std::string& params, uint64_t& depth, bool& allow) {
        std::istringstream in("[params]\n" + params);
        boost::property_tree::ptree tree;
        boost::property_tree::read_ini(in, tree);
        Config::readRollbackGuardSettings(tree.get_child("params"), depth, allow);
    };
    uint64_t depth = 7;
    bool allow = true;
    parse("node_type=client\n", depth, allow);
    EXPECT_EQ(depth, 100u);
    EXPECT_FALSE(allow);
    parse("max_auto_rollback=250\nallow_deep_rollback=true\n", depth, allow);
    EXPECT_EQ(depth, 250u);
    EXPECT_TRUE(allow);
    parse("max_auto_rollback=0\nallow_deep_rollback=false\n", depth, allow);
    EXPECT_EQ(depth, 0u);
    EXPECT_FALSE(allow);
    EXPECT_EQ(Config{}.maxAutoRollbackDepth(), 100u);
    EXPECT_FALSE(Config{}.allowDeepRollback());

    unsetEnv("CS_ALLOW_DEEP_ROLLBACK");
    EXPECT_FALSE(BlockChain::deepRollbackEnvSet());
    setEnv("CS_ALLOW_DEEP_ROLLBACK", "");
    EXPECT_FALSE(BlockChain::deepRollbackEnvSet());  // empty: not set
    setEnv("CS_ALLOW_DEEP_ROLLBACK", "1");
    EXPECT_TRUE(BlockChain::deepRollbackEnvSet());
    unsetEnv("CS_ALLOW_DEEP_ROLLBACK");
}

// allow_deep_rollback (here the environment variable) lifts the bound for one completed episode,
// then it is off for the rest of the run
TEST(BlockChain, DeepRollbackOverrideServesOneEpisode) {
    installDefaultConfig();
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    BlockChain chain(genesis, start);
    chain.test_resetRollbackEpisode();
    setEnv("CS_ALLOW_DEEP_ROLLBACK", "1");

    chain.test_setLastSequence(5000);
    EXPECT_TRUE(chain.deepRollbackApproved());
    ASSERT_TRUE(chain.guardAutoRollback());
    chain.test_setLastSequence(4000);  // far below the bound: allowed by the override
    EXPECT_TRUE(chain.guardAutoRollback());
    EXPECT_FALSE(chain.forkHeld());

    // the episode completes: the override is spent
    chain.test_setLastSequence(5001);
    chain.test_onHeadAdvanced();
    EXPECT_FALSE(chain.test_rollbackEpisode());
    EXPECT_FALSE(chain.deepRollbackApproved());
    chain.test_setLastSequence(5001);
    ASSERT_TRUE(chain.guardAutoRollback());
    chain.test_setLastSequence(5001 - Consensus::MaxAutoRollbackDepth);
    EXPECT_FALSE(chain.guardAutoRollback());
    unsetEnv("CS_ALLOW_DEEP_ROLLBACK");
    chain.test_resetRollbackEpisode();
}

// The episode reference is kept next to the database: a restart neither gives a fresh full depth nor
// forgets FORK HELD
TEST(BlockChain, RollbackReferenceSurvivesRestart) {
    installDefaultConfig();
    TempDir dir("restart");
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    BlockChain chain(genesis, start);
    chain.test_resetRollbackEpisode();
    chain.test_setLastSequence(5000);
    chain.setGuardStateDir(dir.str());
    ASSERT_TRUE(chain.guardAutoRollback());
    ASSERT_TRUE(std::filesystem::exists(dir.path / cs::forkguard::kEpisodeFile));
    chain.test_setLastSequence(4950);
    ASSERT_TRUE(chain.guardAutoRollback());

    // "restart" at head 4950: the reference is still 5000, not 4950
    chain.test_resetRollbackEpisode();
    chain.test_setLastSequence(4950);
    chain.setGuardStateDir(dir.str());
    EXPECT_TRUE(chain.test_rollbackEpisode());
    EXPECT_EQ(chain.test_rollbackHighWater(), 5000u);
    chain.test_setLastSequence(4901);
    EXPECT_TRUE(chain.guardAutoRollback());
    chain.test_setLastSequence(4900);
    EXPECT_FALSE(chain.guardAutoRollback());
    ASSERT_TRUE(chain.forkHeld());
    ASSERT_TRUE(std::filesystem::exists(dir.path / cs::forkguard::kForkFile));

    // restart again: still FORK HELD, same record
    chain.test_resetRollbackEpisode();
    chain.test_setLastSequence(4900);
    chain.setGuardStateDir(dir.str());
    EXPECT_TRUE(chain.forkHeld());
    ASSERT_TRUE(chain.forkHeldRecord().has_value());
    EXPECT_EQ(chain.forkHeldRecord()->highWater, 5000u);
    EXPECT_EQ(chain.forkHeldRecord()->ourHead, 4900u);

    // the chain climbs above the reference: the episode and FORK HELD end, both files go
    chain.test_setLastSequence(5001);
    chain.test_onHeadAdvanced();
    EXPECT_FALSE(chain.forkHeld());
    EXPECT_FALSE(std::filesystem::exists(dir.path / cs::forkguard::kEpisodeFile));
    EXPECT_FALSE(std::filesystem::exists(dir.path / cs::forkguard::kForkFile));

    // a restart above an old reference drops it
    ASSERT_TRUE(cs::forkguard::saveEpisode(dir.str(), {4000}));
    chain.test_resetRollbackEpisode();
    chain.setGuardStateDir(dir.str());
    EXPECT_FALSE(chain.test_rollbackEpisode());
    EXPECT_FALSE(std::filesystem::exists(dir.path / cs::forkguard::kEpisodeFile));
    chain.setGuardStateDir("");
}

// a node holding a fork is offered the other chain's blocks many times a minute: one line at once,
// then at most one a minute with the count since the previous one
TEST(BlockChain, RefusedBlocksAreCountedNotLoggedOneByOne) {
    using namespace std::chrono_literals;
    cs::forkguard::RepeatedLine line;
    const auto t0 = std::chrono::steady_clock::time_point{} + 1h;
    EXPECT_EQ(line.onEvent(t0), 1u);  // the first refusal: a line at once
    for (int i = 1; i < 2000; ++i) {
        EXPECT_EQ(line.onEvent(t0 + std::chrono::milliseconds(25 * i)), 0u);  // 2000 within 50 s
    }
    EXPECT_EQ(line.onEvent(t0 + 60s), 2000u);  // 1999 counted + this one
    EXPECT_EQ(line.onEvent(t0 + 61s), 0u);
    EXPECT_EQ(line.onEvent(t0 + 10min), 2u);  // a quiet spell: the next one at once, with the one held back
    EXPECT_EQ(line.onEvent(t0 + 10min + 1s), 0u);
}

// once FORK HELD, further refusals change nothing on disk: no file is written per refused block
TEST(BlockChain, RefusalsWhileHeldDoNotTouchTheForkFile) {
    installDefaultConfig();
    TempDir dir("refusals");
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    BlockChain chain(genesis, start);
    chain.test_resetRollbackEpisode();
    chain.test_setLastSequence(5000);
    chain.setGuardStateDir(dir.str());
    ASSERT_TRUE(chain.guardAutoRollback());
    chain.test_setLastSequence(4900);
    ASSERT_FALSE(chain.guardAutoRollback());  // beyond the depth bound: FORK HELD
    ASSERT_TRUE(chain.forkHeld());
    const auto file = dir.path / cs::forkguard::kForkFile;
    ASSERT_TRUE(std::filesystem::exists(file));
    const auto written = std::filesystem::last_write_time(file);
    const auto size = std::filesystem::file_size(file);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(chain.test_recordForkHeld("authority"));
    }
    EXPECT_EQ(std::filesystem::last_write_time(file), written);
    EXPECT_EQ(std::filesystem::file_size(file), size);
    EXPECT_TRUE(chain.forkHeld());
    chain.setGuardStateDir("");
}

// a new FORK HELD episode starts its refusal line afresh: its first refusal is logged at once, even
// within a minute of the previous episode's last line
TEST(BlockChain, ANewForkHeldEpisodeLogsItsFirstRefusalAtOnce) {
    using namespace std::chrono_literals;
    installDefaultConfig();
    TempDir dir("refusalepisodes");
    const csdb::Address genesis = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
    const csdb::Address start = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");
    BlockChain chain(genesis, start);
    chain.setGuardStateDir(dir.str());
    const auto t0 = std::chrono::steady_clock::now();
    for (int episode = 0; episode < 2; ++episode) {
        chain.test_resetRollbackEpisode();  // the previous episode ended
        chain.test_setLastSequence(5000);
        ASSERT_TRUE(chain.guardAutoRollback());
        chain.test_setLastSequence(4900);
        ASSERT_FALSE(chain.guardAutoRollback());  // FORK HELD
        ASSERT_TRUE(chain.forkHeld());
        const auto t = t0 + std::chrono::seconds(episode * 5);
        EXPECT_EQ(chain.test_onRefusedBlock(t), 1u) << "episode " << episode;
        EXPECT_EQ(chain.test_onRefusedBlock(t + 1s), 0u);
        EXPECT_EQ(chain.test_onRefusedBlock(t + 2s), 0u);
    }
    chain.setGuardStateDir("");
}

// the fork record and the episode in their files, and the --resolve-fork plan
TEST(BlockChain, ForkGuardFilesRoundTrip) {
    TempDir dir("files");
    EXPECT_EQ(cs::forkguard::stateDirFor("db"), ".");
    EXPECT_EQ(cs::forkguard::stateDirFor("db/"), ".");
    EXPECT_EQ(cs::forkguard::stateDirFor("/data/db"), "/data");

    std::string error;
    EXPECT_FALSE(cs::forkguard::planResolve(dir.str(), &error).has_value());
    EXPECT_NE(error.find("no fork record"), std::string::npos);

    cs::forkguard::ForkRecord r;
    r.reason = "depth";
    r.sinceUnix = 1795000000;
    r.highWater = 5000;
    r.ourHead = 4900;
    r.forkPoint = 4899;
    r.forkPointExact = false;
    r.competingSeq = 4901;
    r.competingHash = "AB";
    r.peerKey = "key";
    r.peerAddress = "1.2.3.4:9510";
    r.peerHeight = 6000;
    ASSERT_TRUE(cs::forkguard::saveForkRecord(dir.str(), r));
    // no hash for the fork point: cannot be checked, refused
    EXPECT_FALSE(cs::forkguard::planResolve(dir.str(), &error).has_value());
    EXPECT_NE(error.find("cannot be checked"), std::string::npos);

    r.forkPointHash = "CD";
    r.dismissedUnix = 1795000100;
    r.dismissedSeq = 4901;
    r.dismissedHash = "AB";
    r.addBranchHash("AB");
    r.addBranchHash("EF");
    r.addBranchHash("AB");  // once only
    ASSERT_TRUE(cs::forkguard::saveForkRecord(dir.str(), r));
    {
        // the Docker healthcheck reads these lines with grep and sed
        std::ifstream in(dir.path / cs::forkguard::kForkFile);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(text.find("\"held\": \"true\""), std::string::npos) << text;
        EXPECT_NE(text.find("\"fork_point\": \"4899\""), std::string::npos) << text;
        EXPECT_NE(text.find("\"since\": \"2026-"), std::string::npos) << text;
    }
    const auto back = cs::forkguard::loadForkRecord(dir.str());
    ASSERT_TRUE(back.has_value());
    EXPECT_TRUE(back->held);
    EXPECT_EQ(back->reason, "depth");
    EXPECT_EQ(back->highWater, 5000u);
    EXPECT_EQ(back->forkPoint, 4899u);
    EXPECT_EQ(back->forkPointHash, "CD");
    EXPECT_EQ(back->competingSeq, 4901u);
    EXPECT_EQ(back->peerAddress, "1.2.3.4:9510");
    EXPECT_EQ(back->peerHeight, 6000u);
    EXPECT_EQ(back->dismissedSeq, 4901u);
    EXPECT_EQ(back->branchHashes, (std::vector<std::string>{"AB", "EF"}));
    EXPECT_TRUE(back->inDismissedBranch("XX", "EF"));
    EXPECT_FALSE(back->inDismissedBranch("XX", "YY"));
    const auto plan = cs::forkguard::planResolve(dir.str(), &error);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->top, 4899u);
    EXPECT_EQ(plan->hash, "CD");

    // the detail line names both choices
    const auto line = cs::forkguard::summary(r);
    EXPECT_NE(line.find("FORK HELD since 2026-"), std::string::npos);
    EXPECT_NE(line.find("--resolve-fork"), std::string::npos);
    EXPECT_NE(line.find("--set-bc-top 4899"), std::string::npos);
    EXPECT_NE(line.find(cs::forkguard::kDismissTrigger), std::string::npos);
    EXPECT_NE(line.find("peer height #6000"), std::string::npos);

    cs::forkguard::clearForkRecord(dir.str());
    EXPECT_FALSE(cs::forkguard::loadForkRecord(dir.str()).has_value());
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
                      uint64_t confirmMask = 0, bool bootstrap = false, bool corruptSignature = false,
                      const csdb::PoolHash& previousHash = csdb::PoolHash{}, const std::string& payload = "payload") {
    csdb::Pool p(previousHash, seq);
    p.set_confidants(keys.pub);
    p.add_number_trusted(static_cast<uint8_t>(keys.pub.size()));
    p.add_real_trusted((1ull << keys.pub.size()) - 1);
    p.add_user_field(200, payload);  // an id no block field uses
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

// an authority refusal becomes FORK HELD when the head stays put over two round starts
void stayStuck(BlockChain& chain) {
    EXPECT_FALSE(chain.promotePendingHold(true));
    EXPECT_TRUE(chain.promotePendingHold(true));
}

// the guard chain in a clean state: no episode, no record, no files
BlockChain& freshGuardChain(const std::string& dir = std::string{}) {
    installDefaultConfig();
    auto& chain = guardChain();
    chain.setGuardStateDir("");
    chain.test_resetRollbackEpisode();
    cs::forkguard::clearForkRecord(dir);
    cs::forkguard::clearEpisode(dir);
    cs::forkguard::clearApprovedBranch(dir);
    chain.setGuardStateDir(dir);
    return chain;
}

csdb::PoolHash someHash(char c) {
    csdb::Pool p(csdb::PoolHash{}, 1);
    p.add_user_field(1, std::string(10, c));
    EXPECT_TRUE(p.compose());
    return p.hash();
}
}  // namespace

// A non-bootstrap competing block may make the node unwind only with confirmations by a majority of
// the confidants of the node's own last block; stricter than block validation (no one-entry masks).
TEST(BlockChain, CompetingBlockNeedsStrictTrustedConfirmations) {
    auto& chain = freshGuardChain();
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

// BlockVerifier accepts a bootstrap block without looking at confirmations: hasTrustedConfirmations
// must not, even when the block carries valid confirmations by our confidants
TEST(BlockChain, BootstrapBlockHasNoTrustedConfirmations) {
    auto& chain = freshGuardChain();
    const auto ours = guardKeys(3);
    const auto next = guardKeys(3);
    chain.test_setLastBlock(guardBlock(40, ours));
    ASSERT_TRUE(chain.hasTrustedConfirmations(guardBlock(41, next, &ours, 0b111)));
    const auto flagged = guardBlock(41, next, &ours, 0b111, true);
    ASSERT_TRUE(BlockChain::isBootstrap(flagged));
    EXPECT_FALSE(chain.hasTrustedConfirmations(flagged));
}

// The bootstrap flag is a bit anyone can set: a bootstrap-flagged competing block passes the gate
// only when it is authenticated locally (valid own signatures AND the confidants of a round table this
// node accepted), never on the flag alone, and never more easily than a normal block.
TEST(BlockChain, BootstrapFlagAloneNeverTriggersUnwind) {
    auto& chain = freshGuardChain();
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

// storeBlock: a competing block the node refuses (not vouched for, or beyond the depth bound) leaves
// the head and the uncertain flag untouched, and raises FORK HELD with a fork record
TEST(BlockChain, RefusedCompetingBlockLeavesTheChainUntouched) {
    TempDir dir("store");
    auto& chain = freshGuardChain(dir.str());
    const auto ours = guardKeys(3);
    const auto next = guardKeys(3);
    const auto strangers = guardKeys(3);

    // not vouched for by our confidants
    const auto own = guardBlock(20, ours);
    chain.test_setLastBlock(own);
    auto foreign = guardBlock(21, next, &strangers, 0b111, false, false, someHash('x'));
    EXPECT_FALSE(chain.storeBlock(foreign, cs::PoolStoreType::Synced));
    EXPECT_EQ(chain.getLastSeq(), 20u);
    EXPECT_EQ(chain.getLastHash(), own.hash());
    EXPECT_FALSE(chain.isLastBlockUncertain());
    EXPECT_FALSE(chain.forkHeld());  // refused at once, reported once the head stays
    EXPECT_TRUE(chain.forkHoldPending());
    stayStuck(chain);
    ASSERT_TRUE(chain.forkHeld());
    auto rec = chain.forkHeldRecord();
    EXPECT_EQ(rec->reason, "authority");
    EXPECT_EQ(rec->competingSeq, 21u);
    EXPECT_EQ(rec->competingHash, foreign.hash().to_string());
    EXPECT_EQ(rec->ourHead, 20u);
    EXPECT_EQ(rec->forkPoint, 19u);  // our #20 is not the competing block's previous one
    EXPECT_FALSE(rec->forkPointExact);
    EXPECT_TRUE(std::filesystem::exists(dir.path / cs::forkguard::kForkFile));

    // vouched for, but beyond the depth bound
    chain.setGuardStateDir("");
    chain.test_resetRollbackEpisode();
    chain.test_setLastSequence(200);
    ASSERT_TRUE(chain.guardAutoRollback());  // reference head #200
    const auto deep = guardBlock(100, ours);
    chain.test_setLastBlock(deep);
    chain.setBlocksToBeRemoved(1);
    auto vouched = guardBlock(101, next, &ours, 0b111, false, false, someHash('y'));
    ASSERT_TRUE(chain.competingBlockMayTriggerUnwind(vouched));
    EXPECT_FALSE(chain.storeBlock(vouched, cs::PoolStoreType::Synced));
    EXPECT_EQ(chain.getLastSeq(), 100u);
    EXPECT_EQ(chain.getLastHash(), deep.hash());
    EXPECT_FALSE(chain.isLastBlockUncertain());
    ASSERT_TRUE(chain.forkHeld());
    EXPECT_EQ(chain.forkHeldRecord()->reason, "depth");
    EXPECT_EQ(chain.forkHeldRecord()->highWater, 200u);
    chain.test_resetRollbackEpisode();
}

// The operator keeps this chain (the dismiss-fork file): FORK HELD ends, the branch stays refused and
// is not reported again, even after a restart; another branch is reported again
TEST(BlockChain, DismissedForkStaysRefusedAndQuiet) {
    TempDir dir("dismiss");
    auto& chain = freshGuardChain(dir.str());
    const auto ours = guardKeys(3);
    const auto next = guardKeys(3);
    const auto strangers = guardKeys(3);
    const auto own = guardBlock(50, ours);
    chain.test_setLastBlock(own);

    auto branch = guardBlock(51, next, &strangers, 0b111, false, false, someHash('a'));
    EXPECT_FALSE(chain.storeBlock(branch, cs::PoolStoreType::Synced));
    EXPECT_FALSE(chain.forkHeld());  // refused at once, reported once the head stays
    EXPECT_TRUE(chain.forkHoldPending());
    stayStuck(chain);
    ASSERT_TRUE(chain.forkHeld());

    // no trigger: nothing happens
    EXPECT_FALSE(chain.checkDismissTrigger());
    EXPECT_TRUE(chain.forkHeld());
    // the trigger file: dismissed and consumed
    { std::ofstream(dir.path / cs::forkguard::kDismissTrigger) << "\n"; }
    EXPECT_TRUE(chain.checkDismissTrigger());
    EXPECT_FALSE(std::filesystem::exists(dir.path / cs::forkguard::kDismissTrigger));
    EXPECT_FALSE(chain.forkHeld());
    const auto rec = cs::forkguard::loadForkRecord(dir.str());
    ASSERT_TRUE(rec.has_value());
    EXPECT_FALSE(rec->held);
    EXPECT_EQ(rec->dismissedSeq, 51u);
    EXPECT_EQ(rec->dismissedHash, branch.hash().to_string());

    // the same branch again: still refused, the chain untouched, no FORK HELD
    EXPECT_FALSE(chain.storeBlock(branch, cs::PoolStoreType::Synced));
    EXPECT_EQ(chain.getLastSeq(), 50u);
    EXPECT_EQ(chain.getLastHash(), own.hash());
    EXPECT_FALSE(chain.forkHeld());
    // a trigger with nothing held is consumed and does nothing
    { std::ofstream(dir.path / cs::forkguard::kDismissTrigger) << "\n"; }
    EXPECT_FALSE(chain.checkDismissTrigger());
    EXPECT_FALSE(std::filesystem::exists(dir.path / cs::forkguard::kDismissTrigger));

    // restart: the dismissal is in the record; the branch, continued by its next block, stays quiet
    chain.setGuardStateDir("");
    chain.test_resetRollbackEpisode();
    chain.test_setLastBlock(guardBlock(51, ours, nullptr, 0, false, false, own.hash()));
    chain.setGuardStateDir(dir.str());
    EXPECT_FALSE(chain.forkHeld());
    ASSERT_TRUE(chain.forkRecord().has_value());
    EXPECT_TRUE(chain.forkRecord()->hasDismissal());
    auto continued = guardBlock(52, next, &strangers, 0b111, false, false, branch.hash());
    EXPECT_FALSE(chain.storeBlock(continued, cs::PoolStoreType::Synced));
    EXPECT_EQ(chain.getLastSeq(), 51u);
    EXPECT_FALSE(chain.forkHeld());

    // a different branch is reported again, and the earlier dismissal is kept
    auto other = guardBlock(52, next, &strangers, 0b111, false, false, someHash('b'), "other");
    EXPECT_FALSE(chain.storeBlock(other, cs::PoolStoreType::Synced));
    EXPECT_FALSE(chain.forkHeld());  // refused at once, reported once the head stays
    EXPECT_TRUE(chain.forkHoldPending());
    stayStuck(chain);
    ASSERT_TRUE(chain.forkHeld());
    EXPECT_EQ(chain.forkHeldRecord()->competingHash, other.hash().to_string());
    EXPECT_TRUE(chain.forkHeldRecord()->inDismissedBranch(branch.hash().to_string(), ""));
    EXPECT_FALSE(chain.forkHeldRecord()->inDismissedBranch(continued.hash().to_string(), ""));  // not vouched for: not added
    chain.setGuardStateDir("");
    chain.test_resetRollbackEpisode();
}

// The antifork selection stopped by the guard leaves a clean state: no pending request, selection
// finished, antifork mode off, the chain as it was, FORK HELD
TEST(BlockChain, AntiforkSelectionAbandonedByTheGuard) {
    auto& chain = freshGuardChain();
    const auto ours = guardKeys(3);
    chain.test_setLastSequence(300);
    ASSERT_TRUE(chain.guardAutoRollback());  // reference head #300
    const auto low = guardBlock(200, ours);
    chain.test_setLastBlock(low);
    chain.test_setSelectionUnfinished(someHash('s'));
    ASSERT_TRUE(chain.getIncorrectBlockNumbers()->empty());

    chain.cacheLastBlocks();
    EXPECT_TRUE(chain.test_selectionFinished());
    EXPECT_FALSE(chain.test_antiForkMode());
    EXPECT_EQ(chain.test_emittingRequest(), 0);
    EXPECT_TRUE(chain.test_lastPrevHash().is_empty());
    EXPECT_TRUE(chain.getIncorrectBlockNumbers()->empty());
    EXPECT_EQ(chain.getLastSeq(), 200u);
    EXPECT_EQ(chain.getLastHash(), low.hash());
    EXPECT_TRUE(chain.forkHeld());
    chain.test_resetRollbackEpisode();
}

// The start-up integrity repair (blocks of incorrectBlocks_) is a local repair, not a network reorg:
// the guard does not bound it and raises nothing, even deep below an episode's reference
TEST(BlockChain, IntegrityRepairIsNotBoundedByTheGuard) {
    auto& chain = freshGuardChain();
    const auto ours = guardKeys(3);
    chain.test_setLastSequence(300);
    ASSERT_TRUE(chain.guardAutoRollback());  // reference head #300: #200 would be refused
    chain.test_setLastBlock(guardBlock(200, ours));
    chain.addIncorrectBlockNumber(200);
    chain.setBlocksToBeRemoved(1);

    chain.cacheLastBlocks();
    EXPECT_EQ(chain.getLastSeq(), 199u);  // the incorrect block was removed
    EXPECT_TRUE(chain.getIncorrectBlockNumbers()->empty());
    EXPECT_FALSE(chain.forkHeld());
    chain.test_resetRollbackEpisode();
}

// The fork point search takes a located point as the fork point only if the peer's block right after
// it verifies as a synced block would after our block there: built on it, signed by its confidants,
// confirmed by a majority of our block's confidants. A bootstrap block (no confirmations), a block
// confirmed by others, or one built on another block stays unverified
TEST(BlockChain, LocatedForkPointNeedsAVerifiedNextBlock) {
    auto& chain = freshGuardChain();
    const auto ours = guardKeys(3);
    const auto theirs = guardKeys(3);
    const auto strangers = guardKeys(3);
    const auto forkBlock = guardBlock(80, ours);
    chain.test_setLastBlock(forkBlock);  // our block at the located point

    const auto verified = guardBlock(81, theirs, &ours, 0b111, false, false, forkBlock.hash());
    EXPECT_TRUE(chain.verifiesAfterOurBlock(verified));
    const auto majority = guardBlock(81, theirs, &ours, 0b011, false, false, forkBlock.hash());
    EXPECT_TRUE(chain.verifiesAfterOurBlock(majority));

    const auto bootstrap = guardBlock(81, theirs, nullptr, 0, true, false, forkBlock.hash());
    EXPECT_FALSE(chain.verifiesAfterOurBlock(bootstrap));
    const auto bootstrapConfirmed = guardBlock(81, theirs, &ours, 0b111, true, false, forkBlock.hash());
    EXPECT_FALSE(chain.verifiesAfterOurBlock(bootstrapConfirmed));
    const auto byStrangers = guardBlock(81, theirs, &strangers, 0b111, false, false, forkBlock.hash());
    EXPECT_FALSE(chain.verifiesAfterOurBlock(byStrangers));
    const auto minority = guardBlock(81, theirs, &ours, 0b001, false, false, forkBlock.hash());
    EXPECT_FALSE(chain.verifiesAfterOurBlock(minority));
    const auto badSignature = guardBlock(81, theirs, &ours, 0b111, false, true, forkBlock.hash());
    EXPECT_FALSE(chain.verifiesAfterOurBlock(badSignature));
    const auto elsewhere = guardBlock(81, theirs, &ours, 0b111, false, false, someHash('e'));
    EXPECT_FALSE(chain.verifiesAfterOurBlock(elsewhere));
    const auto noParent = guardBlock(90, theirs, &ours, 0b111, false, false, forkBlock.hash());
    EXPECT_FALSE(chain.verifiesAfterOurBlock(noParent));  // we store no block at #89
    chain.test_resetRollbackEpisode();
}

// --resolve-fork approves only the recorded block: it alone (that very hash, built on the fork point)
// may replace a block the node stored again right after the fork point; a forged block pointing at it,
// or any other unauthenticated offer, is refused and does not cancel the approval; nothing is ever
// unwound to or below the fork point; the approval clears once the node is past that height
TEST(BlockChain, ResolveForkApprovesOnlyTheRecordedBlock) {
    TempDir dir("approve");
    const auto ours = guardKeys(3);
    const auto theirs = guardKeys(3);
    const auto strangers = guardKeys(3);
    const auto forkBlock = guardBlock(60, ours);                                                     // #60, the fork point
    const auto recorded = guardBlock(61, theirs, &strangers, 0b111, false, false, forkBlock.hash());  // their #61
    const cs::forkguard::ApprovedBranch approval{60, 61, recorded.hash().to_string()};
    const auto ownAgain = guardBlock(61, ours, &ours, 0b111, false, false, forkBlock.hash());         // our #61, stored again

    // unauthenticated offers at #61 and #62: refused as usual, and the approval stays
    {
        auto& chain = freshGuardChain(dir.str());
        ASSERT_TRUE(cs::forkguard::saveApprovedBranch(dir.str(), approval));
        chain.test_setLastBlock(forkBlock);
        chain.setGuardStateDir(dir.str());
        ASSERT_TRUE(chain.approvedBranch().has_value());
        EXPECT_FALSE(chain.deepRollbackApproved());  // nothing as wide as allow_deep_rollback
        auto other = guardBlock(61, strangers, &theirs, 0b111, false, false, someHash('o'));
        EXPECT_FALSE(chain.storeBlock(other, cs::PoolStoreType::Synced));
        EXPECT_EQ(chain.getLastSeq(), 60u);
        EXPECT_EQ(chain.getLastHash(), forkBlock.hash());
        EXPECT_FALSE(chain.forkHeld());  // refused at once, reported once the head stays
        EXPECT_TRUE(chain.forkHoldPending());
        stayStuck(chain);
        EXPECT_TRUE(chain.forkHeld());
        EXPECT_TRUE(chain.approvedBranch().has_value());
        EXPECT_TRUE(std::filesystem::exists(dir.path / cs::forkguard::kApprovedFile));
    }
    // a forged #62 pointing at the recorded hash, while the node holds its own #61 again: refused, the
    // own block kept, the approval kept
    {
        auto& chain = freshGuardChain(dir.str());
        ASSERT_TRUE(cs::forkguard::saveApprovedBranch(dir.str(), approval));
        chain.test_setLastBlock(ownAgain);
        chain.setGuardStateDir(dir.str());
        auto forged = guardBlock(62, strangers, &theirs, 0b111, false, false, recorded.hash());
        EXPECT_FALSE(chain.storeBlock(forged, cs::PoolStoreType::Synced));
        EXPECT_EQ(chain.getLastSeq(), 61u);
        EXPECT_EQ(chain.getLastHash(), ownAgain.hash());
        EXPECT_FALSE(chain.isLastBlockUncertain());
        EXPECT_FALSE(chain.forkHeld());  // refused at once, reported once the head stays
        EXPECT_TRUE(chain.forkHoldPending());
        stayStuck(chain);
        EXPECT_TRUE(chain.forkHeld());
        EXPECT_TRUE(chain.approvedBranch().has_value());
    }
    // nothing is unwound to or below the fork point while the approval is pending
    {
        auto& chain = freshGuardChain(dir.str());
        ASSERT_TRUE(cs::forkguard::saveApprovedBranch(dir.str(), approval));
        chain.test_setLastBlock(forkBlock);
        chain.setGuardStateDir(dir.str());
        chain.setBlocksToBeRemoved(1);
        EXPECT_FALSE(chain.guardAutoRollback());
        chain.removeLastBlock();
        EXPECT_EQ(chain.getLastSeq(), 60u);
        EXPECT_EQ(chain.getLastHash(), forkBlock.hash());
        EXPECT_TRUE(chain.approvedBranch().has_value());
    }
    // the recorded block: it replaces the own #61, down to the fork point and no further, then the
    // node appends it and the approval clears
    {
        auto& chain = freshGuardChain(dir.str());
        ASSERT_TRUE(cs::forkguard::saveApprovedBranch(dir.str(), approval));
        chain.test_setLastBlock(ownAgain);
        chain.setGuardStateDir(dir.str());
        chain.setBlocksToBeRemoved(1);
        auto offered = recorded;
        EXPECT_FALSE(chain.storeBlock(offered, cs::PoolStoreType::Synced));
        EXPECT_EQ(chain.getLastSeq(), 60u);  // own #61 removed, the fork point kept
        EXPECT_FALSE(chain.forkHeld());
        EXPECT_TRUE(chain.approvedBranch().has_value());
        EXPECT_FALSE(chain.guardAutoRollback());  // and never below it

        chain.test_setLastBlock(recorded);  // appended as the next block
        chain.test_onHeadAdvanced();
        EXPECT_FALSE(chain.approvedBranch().has_value());
        EXPECT_FALSE(std::filesystem::exists(dir.path / cs::forkguard::kApprovedFile));
    }
    // a competing block vouched for by this node's confidants at that height ends the approval
    {
        auto& chain = freshGuardChain(dir.str());
        ASSERT_TRUE(cs::forkguard::saveApprovedBranch(dir.str(), approval));
        chain.test_setLastBlock(ownAgain);
        chain.setGuardStateDir(dir.str());
        auto vouched = guardBlock(62, theirs, &ours, 0b111, false, false, someHash('v'));
        ASSERT_TRUE(chain.competingBlockMayTriggerUnwind(vouched));
        chain.storeBlock(vouched, cs::PoolStoreType::Synced);
        EXPECT_FALSE(chain.approvedBranch().has_value());
    }
    // the node goes past that height with another block: the approval clears too
    {
        auto& chain = freshGuardChain(dir.str());
        ASSERT_TRUE(cs::forkguard::saveApprovedBranch(dir.str(), approval));
        chain.test_setLastBlock(forkBlock);
        chain.setGuardStateDir(dir.str());
        chain.test_setLastBlock(guardBlock(62, ours));
        chain.test_onHeadAdvanced();
        EXPECT_FALSE(chain.approvedBranch().has_value());
    }
    freshGuardChain();
}

// After --resolve-fork the branch the operator chose is preferred once: at the height after the fork
// point only the chosen block is taken; the abandoned block (and anything else there) is refused, also
// in the cache one height up; the preference clears once the chosen block is stored, or times out (the
// abandoned block alone then stays refused); it survives a restart; it never lets a block through the
// authority gate
TEST(BlockChain, ResolveForkPrefersTheChosenBranch) {
    TempDir dir("prefer");
    const auto ours = guardKeys(3);
    const auto theirs = guardKeys(3);
    const auto strangers = guardKeys(3);
    const auto forkBlock = guardBlock(60, ours);                                                              // the cut
    const auto chosen = guardBlock(61, theirs, &ours, 0b111, false, false, forkBlock.hash(), "chosen");        // their #61
    const auto abandoned = guardBlock(61, ours, &ours, 0b111, false, false, forkBlock.hash(), "abandoned");    // our old #61
    const auto third = guardBlock(61, strangers, &ours, 0b111, false, false, forkBlock.hash(), "third");
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    cs::forkguard::ApprovedBranch approval;
    approval.forkPoint = 60;
    approval.seq = 61;
    approval.hash = chosen.hash().to_string();
    approval.abandonedHash = abandoned.hash().to_string();
    approval.peerKey = "peer";
    approval.untilUnix = now + cs::forkguard::kPreferenceSeconds;

    using cs::forkguard::BranchVerdict;
    EXPECT_EQ(cs::forkguard::judgeBlock(approval, 61, approval.hash), BranchVerdict::Chosen);
    EXPECT_EQ(cs::forkguard::judgeBlock(approval, 61, approval.abandonedHash), BranchVerdict::Refuse);
    EXPECT_EQ(cs::forkguard::judgeBlock(approval, 61, third.hash().to_string()), BranchVerdict::Refuse);
    EXPECT_EQ(cs::forkguard::judgeBlock(approval, 62, third.hash().to_string()), BranchVerdict::None);
    EXPECT_EQ(cs::forkguard::judgeChild(approval, 62, approval.hash), BranchVerdict::Chosen);
    EXPECT_EQ(cs::forkguard::judgeChild(approval, 62, approval.abandonedHash), BranchVerdict::Refuse);
    EXPECT_EQ(cs::forkguard::judgeChild(approval, 63, approval.abandonedHash), BranchVerdict::None);

    // the abandoned block (even vouched for by our confidants) and any other block at #61: refused;
    // a block of the abandoned branch at #62 is not cached
    {
        auto& chain = freshGuardChain(dir.str());
        ASSERT_TRUE(cs::forkguard::saveApprovedBranch(dir.str(), approval));
        chain.test_setLastBlock(forkBlock);
        chain.setGuardStateDir(dir.str());
        ASSERT_TRUE(chain.approvedBranch().has_value());
        EXPECT_TRUE(chain.approvedBranch()->preferring());
        EXPECT_FALSE(chain.abandonedCacheDropPending());
        // a block nobody vouches for, refused there: anyone can send it, so it asks for no cache drop
        auto forged = guardBlock(61, strangers, &strangers, 0b111, false, false, forkBlock.hash(), "forged");
        EXPECT_FALSE(chain.storeBlock(forged, cs::PoolStoreType::Synced));
        EXPECT_EQ(chain.getLastSeq(), 60u);
        EXPECT_FALSE(chain.abandonedCacheDropPending());
        for (const auto* b : {&abandoned, &third}) {
            auto offered = *b;
            EXPECT_FALSE(chain.storeBlock(offered, cs::PoolStoreType::Synced));
            EXPECT_EQ(chain.getLastSeq(), 60u);
            EXPECT_EQ(chain.getLastHash(), forkBlock.hash());
        }
        EXPECT_TRUE(chain.abandonedCacheDropPending());  // the abandoned block itself
        auto child = guardBlock(62, ours, &ours, 0b111, false, false, abandoned.hash());
        const auto cached = chain.getCachedBlocksSize();
        EXPECT_FALSE(chain.storeBlock(child, cs::PoolStoreType::Synced));
        EXPECT_EQ(chain.getCachedBlocksSize(), cached);
        // the cache: a block built on that refused child (abandoned branch) goes, an unrelated one stays
        auto grandchild = guardBlock(63, ours, &ours, 0b111, false, false, child.hash(), "grandchild");
        auto unrelatedCached = guardBlock(64, theirs, &theirs, 0b111, false, false, someHash('k'), "kept");
        EXPECT_TRUE(chain.storeBlock(grandchild, cs::PoolStoreType::Synced));
        EXPECT_TRUE(chain.storeBlock(unrelatedCached, cs::PoolStoreType::Synced));
        EXPECT_EQ(chain.getCachedBlocksSize(), cached + 2);
        EXPECT_EQ(chain.dropAbandonedFromCache(), 1u);
        EXPECT_EQ(chain.getCachedBlocksSize(), cached + 1);
        EXPECT_FALSE(chain.abandonedCacheDropPending());
        // once per preference: another abandoned offer asks for no second drop
        auto again = abandoned;
        EXPECT_FALSE(chain.storeBlock(again, cs::PoolStoreType::Synced));
        EXPECT_FALSE(chain.abandonedCacheDropPending());
        EXPECT_EQ(chain.dropAbandonedFromCache(), 0u);
        EXPECT_EQ(chain.getCachedBlocksSize(), cached + 1);
        chain.clearBlockCache();
        EXPECT_FALSE(chain.forkHeld());
        EXPECT_FALSE(chain.forkHoldPending());  // refused by the preference, no fork to report
        EXPECT_TRUE(chain.approvedBranch().has_value());

        // no relaxation: a competing block not built on our head still meets the authority gate
        auto unrelated = guardBlock(61, strangers, &theirs, 0b111, false, false, someHash('u'));
        EXPECT_FALSE(chain.competingBlockMayTriggerUnwind(unrelated));
        EXPECT_FALSE(chain.storeBlock(unrelated, cs::PoolStoreType::Synced));
        EXPECT_EQ(chain.getLastSeq(), 60u);
        EXPECT_TRUE(chain.forkHoldPending());
        EXPECT_FALSE(chain.deepRollbackApproved());

        // the chosen block is taken (it still goes through the usual checks), and then the
        // preference ends, the file goes and the cache is to be dropped
        EXPECT_EQ(cs::forkguard::judgeBlock(*chain.approvedBranch(), 61, chosen.hash().to_string()), BranchVerdict::Chosen);
        chain.test_setLastBlock(chosen);
        chain.test_onHeadAdvanced();
        EXPECT_FALSE(chain.approvedBranch().has_value());
        EXPECT_FALSE(std::filesystem::exists(dir.path / cs::forkguard::kApprovedFile));
    }
    // the chosen block stored before any drop: one drop then, of the abandoned descendants only
    {
        auto& chain = freshGuardChain(dir.str());
        ASSERT_TRUE(cs::forkguard::saveApprovedBranch(dir.str(), approval));
        chain.test_setLastBlock(forkBlock);
        chain.setGuardStateDir(dir.str());
        EXPECT_FALSE(chain.abandonedCacheDropPending());
        chain.test_setLastBlock(chosen);
        chain.test_onHeadAdvanced();
        EXPECT_FALSE(chain.approvedBranch().has_value());
        EXPECT_TRUE(chain.abandonedCacheDropPending());
        EXPECT_EQ(chain.dropAbandonedFromCache(), 0u);  // nothing of that branch cached
        EXPECT_FALSE(chain.abandonedCacheDropPending());
    }
    // a restart keeps the preference (the file), and its deadline
    {
        auto& chain = freshGuardChain(dir.str());
        ASSERT_TRUE(cs::forkguard::saveApprovedBranch(dir.str(), approval));
        chain.test_setLastBlock(forkBlock);
        chain.setGuardStateDir(dir.str());
        chain.setGuardStateDir("");
        chain.test_resetRollbackEpisode();
        chain.test_setLastBlock(forkBlock);
        chain.setGuardStateDir(dir.str());
        const auto loaded = chain.approvedBranch();
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->hash, approval.hash);
        EXPECT_EQ(loaded->abandonedHash, approval.abandonedHash);
        EXPECT_EQ(loaded->peerKey, "peer");
        EXPECT_EQ(loaded->untilUnix, approval.untilUnix);
        EXPECT_TRUE(loaded->preferring());
        chain.checkResolvePreference(now);  // not due yet
        EXPECT_TRUE(chain.approvedBranch()->preferring());
    }
    // the deadline passes: the preference ends (logged, persisted), the abandoned block stays refused,
    // another block there is no longer refused by it; after a restart too
    {
        auto& chain = freshGuardChain(dir.str());
        ASSERT_TRUE(cs::forkguard::saveApprovedBranch(dir.str(), approval));
        chain.test_setLastBlock(forkBlock);
        chain.setGuardStateDir(dir.str());
        chain.checkResolvePreference(approval.untilUnix + 1);
        ASSERT_TRUE(chain.approvedBranch().has_value());
        EXPECT_FALSE(chain.approvedBranch()->preferring());
        EXPECT_TRUE(chain.approvedBranch()->expired);
        EXPECT_EQ(cs::forkguard::judgeBlock(*chain.approvedBranch(), 61, third.hash().to_string()), BranchVerdict::None);
        auto again = abandoned;
        EXPECT_FALSE(chain.storeBlock(again, cs::PoolStoreType::Synced));
        EXPECT_EQ(chain.getLastSeq(), 60u);
        const auto file = cs::forkguard::loadApprovedBranch(dir.str());
        ASSERT_TRUE(file.has_value());
        EXPECT_TRUE(file->expired);
        EXPECT_EQ(file->abandonedHash, approval.abandonedHash);
        chain.checkResolvePreference(approval.untilUnix + 100);  // once only
        EXPECT_TRUE(chain.approvedBranch().has_value());
    }
    // no abandoned block known: the timeout ends the approval altogether
    {
        auto& chain = freshGuardChain(dir.str());
        auto noAbandoned = approval;
        noAbandoned.abandonedHash.clear();
        ASSERT_TRUE(cs::forkguard::saveApprovedBranch(dir.str(), noAbandoned));
        chain.test_setLastBlock(forkBlock);
        chain.setGuardStateDir(dir.str());
        chain.checkResolvePreference(approval.untilUnix);
        EXPECT_FALSE(chain.approvedBranch().has_value());
        EXPECT_FALSE(std::filesystem::exists(dir.path / cs::forkguard::kApprovedFile));
    }
    // an approval file of an earlier version (no deadline): an approval without preference
    {
        auto& chain = freshGuardChain(dir.str());
        ASSERT_TRUE(cs::forkguard::saveApprovedBranch(dir.str(), {60, 61, approval.hash}));
        chain.test_setLastBlock(forkBlock);
        chain.setGuardStateDir(dir.str());
        ASSERT_TRUE(chain.approvedBranch().has_value());
        EXPECT_FALSE(chain.approvedBranch()->preferring());
        EXPECT_EQ(cs::forkguard::judgeBlock(*chain.approvedBranch(), 61, third.hash().to_string()), BranchVerdict::None);
    }
    freshGuardChain();
}

// --resolve-fork acts only on a held fork: a dismissed record is refused, with the remedy
TEST(BlockChain, ResolvePlanRefusesADismissedRecord) {
    TempDir dir("plan-dismissed");
    cs::forkguard::ForkRecord r;
    r.held = false;
    r.reason = "authority";
    r.forkPoint = 99;
    r.forkPointHash = "AB";
    r.dismissedUnix = 1795000000;
    r.dismissedSeq = 100;
    r.dismissedHash = "CD";
    r.addBranchHash("CD");
    ASSERT_TRUE(cs::forkguard::saveForkRecord(dir.str(), r));
    std::string error;
    EXPECT_FALSE(cs::forkguard::planResolve(dir.str(), &error).has_value());
    EXPECT_NE(error.find("dismissed on 2026-"), std::string::npos) << error;
    EXPECT_NE(error.find("--set-bc-top <sequence>"), std::string::npos) << error;
}

// an unreadable (or empty) file of each kind is reported, never read as "nothing there": the episode
// fails safe (a new one at the head), the fork record stays FORK HELD, an approval is dropped
TEST(BlockChain, CorruptGuardFilesFailSafe) {
    for (const std::string content : {std::string("{ not json"), std::string()}) {
        SCOPED_TRACE(content.empty() ? "empty" : "garbage");
        TempDir dir("corrupt");
        const auto write = [&](const char* name) { std::ofstream(dir.path / name, std::ios::binary) << content; };
        bool corrupt = false;

        write(cs::forkguard::kEpisodeFile);
        EXPECT_FALSE(cs::forkguard::loadEpisode(dir.str(), &corrupt).has_value());
        EXPECT_TRUE(corrupt);
        write(cs::forkguard::kForkFile);
        EXPECT_FALSE(cs::forkguard::loadForkRecord(dir.str(), &corrupt).has_value());
        EXPECT_TRUE(corrupt);
        std::string error;
        EXPECT_FALSE(cs::forkguard::planResolve(dir.str(), &error).has_value());
        EXPECT_NE(error.find("cannot be read"), std::string::npos) << error;
        write(cs::forkguard::kApprovedFile);
        EXPECT_FALSE(cs::forkguard::loadApprovedBranch(dir.str(), &corrupt).has_value());
        EXPECT_TRUE(corrupt);
        // a missing file is not corrupt
        EXPECT_FALSE(cs::forkguard::loadEpisode((dir.path / "none").string(), &corrupt).has_value());
        EXPECT_FALSE(corrupt);

        auto& chain = freshGuardChain();
        chain.test_setLastSequence(700);
        write(cs::forkguard::kEpisodeFile);
        write(cs::forkguard::kForkFile);
        write(cs::forkguard::kApprovedFile);
        chain.setGuardStateDir(dir.str());
        EXPECT_TRUE(chain.test_rollbackEpisode());          // a new episode at the head
        EXPECT_EQ(chain.test_rollbackHighWater(), 700u);
        const auto episode = cs::forkguard::loadEpisode(dir.str(), &corrupt);
        ASSERT_TRUE(episode.has_value());
        EXPECT_EQ(episode->highWater, 700u);
        ASSERT_TRUE(chain.forkHeld());                     // the lost record: still FORK HELD
        EXPECT_EQ(chain.forkHeldRecord()->reason, "unreadable");
        EXPECT_FALSE(cs::forkguard::planResolve(dir.str(), &error).has_value());  // no hash to check
        EXPECT_FALSE(chain.approvedBranch().has_value());  // no approval
        EXPECT_FALSE(std::filesystem::exists(dir.path / cs::forkguard::kApprovedFile));
        freshGuardChain();
    }
}

// a restart below the floor of the persisted reference refuses at once; and the files go together
// when the database they describe is replaced
TEST(BlockChain, RestartBelowTheFloorAndCleanup) {
    TempDir dir("floor");
    auto& chain = freshGuardChain();
    ASSERT_TRUE(cs::forkguard::saveEpisode(dir.str(), {5000}));
    chain.test_setLastSequence(4800);  // already below 5000 - 100
    chain.setGuardStateDir(dir.str());
    EXPECT_EQ(chain.test_rollbackHighWater(), 5000u);
    EXPECT_FALSE(chain.guardAutoRollback());
    EXPECT_TRUE(chain.forkHeld());
    ASSERT_TRUE(cs::forkguard::saveApprovedBranch(dir.str(), {10, 11, "AB"}));
    for (const char* f : {cs::forkguard::kEpisodeFile, cs::forkguard::kForkFile, cs::forkguard::kApprovedFile}) {
        EXPECT_TRUE(std::filesystem::exists(dir.path / f)) << f;
    }
    cs::forkguard::clearAll(dir.str());
    for (const char* f : {cs::forkguard::kEpisodeFile, cs::forkguard::kForkFile, cs::forkguard::kApprovedFile}) {
        EXPECT_FALSE(std::filesystem::exists(dir.path / f)) << f;
    }
    freshGuardChain();
}

// a block not vouched for, refused while the chain goes on: no FORK HELD, no file, no flap
TEST(BlockChain, AuthorityRefusalDoesNotFlap) {
    TempDir dir("flap");
    auto& chain = freshGuardChain(dir.str());
    const auto ours = guardKeys(3);
    const auto next = guardKeys(3);
    const auto strangers = guardKeys(3);
    const auto own = guardBlock(70, ours);
    chain.test_setLastBlock(own);
    auto stray = guardBlock(71, next, &strangers, 0b111, false, false, someHash('f'));
    EXPECT_FALSE(chain.storeBlock(stray, cs::PoolStoreType::Synced));  // refused at once
    EXPECT_TRUE(chain.forkHoldPending());
    EXPECT_FALSE(chain.forkHeld());
    EXPECT_FALSE(std::filesystem::exists(dir.path / cs::forkguard::kForkFile));
    EXPECT_FALSE(std::filesystem::exists(dir.path / cs::forkguard::kEpisodeFile));
    EXPECT_FALSE(chain.promotePendingHold(true));  // one round start: not yet

    // the next block of our chain: the refusal is forgotten
    chain.test_setLastBlock(guardBlock(71, ours, &ours, 0b111, false, false, own.hash()));
    chain.test_onHeadAdvanced();
    EXPECT_FALSE(chain.forkHoldPending());
    EXPECT_FALSE(chain.promotePendingHold(true));
    EXPECT_FALSE(chain.promotePendingHold(true));
    EXPECT_FALSE(chain.forkHeld());
    for (const char* f : {cs::forkguard::kEpisodeFile, cs::forkguard::kForkFile, cs::forkguard::kApprovedFile}) {
        EXPECT_FALSE(std::filesystem::exists(dir.path / f)) << f;
    }
    freshGuardChain();
}

// forged blocks cannot push the dismissed block out of the remembered hashes
TEST(BlockChain, DismissedHashIsNeverEvicted) {
    cs::forkguard::ForkRecord r;
    r.dismissedHash = "D";
    r.addBranchHash("D");
    for (size_t i = 0; i < cs::forkguard::kMaxBranchHashes * 2; ++i) {
        r.addBranchHash("F" + std::to_string(i));
    }
    EXPECT_EQ(r.branchHashes.size(), cs::forkguard::kMaxBranchHashes);
    EXPECT_TRUE(r.inDismissedBranch("D", ""));
    EXPECT_TRUE(r.inDismissedBranch("x", "D"));
    EXPECT_FALSE(r.inDismissedBranch("F0", ""));  // the oldest others did go
}
