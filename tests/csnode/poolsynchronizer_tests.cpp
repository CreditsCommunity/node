#include <gtest/gtest.h>

#include <csnode/blockchain.hpp>
#include <csnode/poolsynchronizer.hpp>

#include <lib/system/random.hpp>

const csdb::Address genesisAddress = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001");
const csdb::Address startAddress = csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002");

[[maybe_unused]] static std::vector<std::string> base58Keys = {
    "4tEQbQPYZq1bZ8Tn9DpCXYUgPgEgcqsBPXX4fXef7FuL",
    "CzvqNvaQpsE2v4FFTJ2mnTKu48JsMhLzzMALja8bt7DD",
    "G2GeLfwjg6XuvoWnZ7ssx9EPkEBqbYL3mw3fusgpzoBk",
    "HYNKVYEC5pKAAgoJ56WdvbuzJBpjZtGBvpSMBeVd555S",
    "FBpAc4VDxVk1oDLK73xYmCwMJC2XA9zGyXjxJeN15KuD",
    "132jBPFmZhWn4siSAKXHztXizTRmBVjTuy7ejwiW3jcu",
    "4VYZzAWuT4K5iti9Xjckyb196jtu9oaCnrkAgXHHt7t6",
    "DDhaSqP3U67Cjs1fm7HtdFpErZUgMJ4CCZqgzdxYGoRe"
};

class PoolSynchronizerTest : public cs::PoolSynchronizer {
public:
    explicit PoolSynchronizerTest(BlockChain* blockchain):PoolSynchronizer(blockchain) {}
};

static std::unique_ptr<PoolSynchronizerTest> createPoolSynchronizer() {
    static BlockChain blockChain(genesisAddress, startAddress);
    return std::make_unique<PoolSynchronizerTest>(&blockChain);
}

static std::map<cs::PublicKey, cs::Sequence> createKeys() {
    static const size_t keysCount = 100;
    std::map<cs::PublicKey, cs::Sequence> keys;

    for (size_t i = 0; i < keysCount; ++i) {
        cs::PublicKey key;
        cscrypto::generateKeyPair(key);

        keys.emplace(key, 1);
    }

    return keys;
}

TEST(PoolSynchronizer, DISABLED_TestNeighbours) {
    static const size_t iterations = 1000;

    auto synchronizer = createPoolSynchronizer();
    auto keys = createKeys();

    for (const auto& [key, value] : keys) {
        synchronizer->onNeighbourAdded(key, value);
    }

    for (size_t i = 0; i < iterations; ++i) {
        auto index = cs::Random::generateValue<size_t>(0, 99);
        auto iter = std::next(keys.begin(), static_cast<std::ptrdiff_t>(index));
        auto sequenceStep = cs::Random::generateValue<cs::Sequence>(10, 1000);

        auto& [key, sequence] = *iter;
        sequence += sequenceStep;

        // neighbours are no longer kept sorted (unordered_map since 3ed6b98b), so only
        // check that ping handling does not fail
        synchronizer->onPingReceived(sequence, key);
    }
}

// after --resolve-fork the sync asks a preferred source first, but a random one of them; a source that
// brings no block within the timeout is demoted; with every source demoted (or none usable) the usual
// random choice applies, so a peer claiming a version or a height cannot hold the sync
TEST(PreferredSources, ShuffledDemotedAndFallBack) {
    using namespace std::chrono_literals;
    auto key = [](uint8_t b) {
        cs::PublicKey k{};
        k.fill(b);
        return k;
    };
    cs::PreferredSources pref(20s);
    const std::function<bool(const cs::PublicKey&)> all = [](const cs::PublicKey&) { return true; };
    const std::function<size_t(size_t)> r0 = [](size_t) { return size_t{0}; };
    const std::function<size_t(size_t)> r1 = [](size_t) { return size_t{1}; };
    const std::function<size_t(size_t)> r2 = [](size_t) { return size_t{2}; };
    auto t = cs::PreferredSources::Clock::time_point{} + 1h;
    EXPECT_FALSE(pref.pick(all, 100, t, r0).has_value());  // none set

    pref.set({key(1), key(2), key(3), key(4)});
    EXPECT_EQ(pref.sources().size(), cs::PreferredSources::kMax);  // a few at most
    // random among the usable ones (not always the first)
    EXPECT_EQ(*pref.pick(all, 100, t, r2), key(3));
    EXPECT_EQ(*pref.pick(all, 100, t, r1), key(2));
    // unusable (not a neighbour, nothing above ours): skipped
    const std::function<bool(const cs::PublicKey&)> not2 = [&](const cs::PublicKey& k) { return k != key(2); };
    EXPECT_EQ(*pref.pick(not2, 100, t, r0), key(1));
    // progress keeps a source
    t += 15s;
    EXPECT_EQ(*pref.pick(all, 101, t, r0), key(1));
    t += 15s;
    EXPECT_EQ(*pref.pick(all, 102, t, r0), key(1));
    EXPECT_FALSE(pref.demoted(key(1)));
    // no block within the timeout: demoted, another source
    t += 21s;
    EXPECT_EQ(*pref.pick(all, 102, t, r0), key(2));
    EXPECT_TRUE(pref.demoted(key(1)));
    t += 21s;
    EXPECT_EQ(*pref.pick(all, 102, t, r0), key(3));
    t += 21s;
    // every source demoted: the usual random choice
    EXPECT_FALSE(pref.pick(all, 102, t, r0).has_value());
    // the same list again keeps the demotions; an empty list ends the preference
    pref.set({key(1), key(2), key(3)});
    EXPECT_FALSE(pref.pick(all, 102, t, r0).has_value());
    pref.set({});
    pref.set({key(1)});
    EXPECT_EQ(*pref.pick(all, 102, t, r0), key(1));
}
