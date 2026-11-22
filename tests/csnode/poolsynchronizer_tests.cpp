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

// after --resolve-fork the sync asks a preferred source first, but a random one of them, and keeps it
// while it brings blocks; a source that brings none within the timeout, or answers without moving our
// head, is demoted; with every source demoted (or none usable) the usual random choice applies. The
// list is fixed at the start (it only grows while there is room): reordering it, as the node's rebuild
// every few seconds does, restarts nothing, so a peer claiming a version or a height cannot hold the sync
TEST(PreferredSources, ShuffledDemotedAndFallBack) {
    using namespace std::chrono_literals;
    auto key = [](uint8_t b) {
        cs::PublicKey k{};
        k.fill(b);
        return k;
    };
    const std::function<bool(const cs::PublicKey&)> all = [](const cs::PublicKey&) { return true; };
    const std::function<size_t(size_t)> r0 = [](size_t) { return size_t{0}; };
    const std::function<size_t(size_t)> r1 = [](size_t) { return size_t{1}; };
    const std::function<size_t(size_t)> r2 = [](size_t) { return size_t{2}; };
    auto t = cs::PreferredSources::Clock::time_point{} + 1h;
    {
        cs::PreferredSources pref(20s);
        EXPECT_FALSE(pref.pick(all, 100, t, r0).has_value());  // none set
        pref.set({key(1), key(2), key(3), key(4)});
        EXPECT_EQ(pref.sources().size(), cs::PreferredSources::kMax);  // a few at most
        // a random one of the usable sources (not the first claimed)
        EXPECT_EQ(*pref.pick(all, 100, t, r2), key(3));
        // kept while it brings blocks, whatever the random draw
        t += 15s;
        EXPECT_EQ(*pref.pick(all, 101, t, r0), key(3));
        t += 15s;
        EXPECT_EQ(*pref.pick(all, 102, t, r1), key(3));
        EXPECT_FALSE(pref.demoted(key(3)));
        // the node rebuilds the list every 5 s in another order: no new window, demoted after 20 s
        // without a block all the same
        for (int i = 0; i < 4; ++i) {
            pref.set(i % 2 ? std::vector<cs::PublicKey>{key(2), key(1), key(3)} : std::vector<cs::PublicKey>{key(3), key(2), key(1)});
            t += 5s;
            pref.pick(all, 102, t, r0);
        }
        t += 1s;
        EXPECT_NE(*pref.pick(all, 102, t, r0), key(3));
        EXPECT_TRUE(pref.demoted(key(3)));
        // a new list does not bring new members once full, and does not reorder
        pref.set({key(9), key(8)});
        EXPECT_EQ(pref.sources(), (std::vector<cs::PublicKey>{key(1), key(2), key(3)}));
    }
    {
        // answered, but our head did not move: demoted at once
        cs::PreferredSources pref(20s);
        pref.set({key(1), key(2)});
        EXPECT_EQ(*pref.pick(all, 100, t, r0), key(1));
        pref.onReply(key(2), false);  // not the source asked: nothing
        EXPECT_FALSE(pref.demoted(key(2)));
        pref.onReply(key(1), true);
        EXPECT_FALSE(pref.demoted(key(1)));
        pref.onReply(key(1), false);
        EXPECT_TRUE(pref.demoted(key(1)));
        EXPECT_EQ(*pref.pick(all, 100, t, r0), key(2));
        t += 21s;
        // every source demoted: the usual random choice
        EXPECT_FALSE(pref.pick(all, 100, t, r0).has_value());
        EXPECT_TRUE(pref.demoted(key(2)));
        // the same members again keep the demotions; an empty list ends the preference
        pref.set({key(2), key(1)});
        EXPECT_FALSE(pref.pick(all, 100, t, r0).has_value());
        pref.set({});
        pref.set({key(1)});
        EXPECT_EQ(*pref.pick(all, 100, t, r0), key(1));
    }
    {
        // unusable sources (not neighbours, nothing above ours) are skipped; the list grows while
        // there is room (a neighbour that connects later)
        cs::PreferredSources pref(20s);
        pref.set({key(1)});
        const std::function<bool(const cs::PublicKey&)> not1 = [&](const cs::PublicKey& k) { return k != key(1); };
        EXPECT_FALSE(pref.pick(not1, 100, t, r0).has_value());
        pref.set({key(1), key(5)});
        EXPECT_EQ(*pref.pick(not1, 100, t, r0), key(5));
    }
}
