#include "gtest/gtest.h"

#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <csdb/database.hpp>
#include <csdb/pool.hpp>
#include <csdb/storage.hpp>

#include "memorydb.hpp"

// The asynchronous block writer of csdb::Storage (used with RocksDB), on an in-memory database whose
// put_batch can be held or made to fail.
namespace {

using namespace std::chrono_literals;

std::vector<csdb::Pool> chain(size_t count) {
    std::vector<csdb::Pool> pools;
    csdb::PoolHash prev;
    for (size_t i = 0; i < count; ++i) {
        csdb::Pool p(prev, i);
        p.add_user_field(1, std::string(100, static_cast<char>('a' + i)));
        EXPECT_TRUE(p.compose());
        prev = p.hash();
        pools.push_back(p);
    }
    return pools;
}

bool openAsync(csdb::Storage& storage, const std::shared_ptr<MemoryDb>& db) {
    csdb::Storage::OpenOptions opt;
    opt.db = db;
    opt.writeBatchSize = 100;
    opt.asyncWrites = true;
    return storage.open(opt);
}

// runs f on its own thread; false if it did not return within the limit (the thread is left behind)
template <typename F>
bool returnsWithin(F f, std::chrono::milliseconds limit) {
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    std::thread([f, done]() mutable { f(); done->set_value(); }).detach();
    return fut.wait_for(limit) == std::future_status::ready;
}

}  // namespace

// The last block is in the batch being written when the node removes it (fork resolution): it must
// end up removed from the database too, and the writer must not pop more than it took.
TEST(StorageWriter, RemovingTheLastBlockWhileItIsWrittenRemovesItFromTheDatabase) {
    auto db = std::make_shared<MemoryDb>();
    csdb::Storage storage;
    ASSERT_TRUE(openAsync(storage, db));
    const auto pools = chain(4);

    db->hold();
    ASSERT_TRUE(storage.pool_save(pools[0]));
    ASSERT_TRUE(db->waitCalls(1));          // the writer holds [0]
    for (size_t i = 1; i < 4; ++i) ASSERT_TRUE(storage.pool_save(pools[i]));
    db->permit(1);                          // [0] lands, the writer takes [1, 2, 3]
    ASSERT_TRUE(db->waitCalls(2));

    std::promise<csdb::Pool> removed;
    auto removedFut = removed.get_future();
    std::thread remover([&] { removed.set_value(storage.pool_remove_last()); });
    std::this_thread::sleep_for(100ms);     // the removal runs while [1, 2, 3] is being written
    db->release();
    remover.join();

    const csdb::Pool last = removedFut.get();
    ASSERT_TRUE(last.is_valid());
    EXPECT_EQ(last.hash(), pools[3].hash());
    ASSERT_TRUE(storage.flush());
    EXPECT_FALSE(db->has(pools[3].hash())) << "a removed block was written anyway";
    EXPECT_TRUE(db->has(pools[2].hash()));
    EXPECT_EQ(storage.size(), 3u);
    EXPECT_EQ(storage.last_hash(), pools[2].hash());
    EXPECT_EQ(db->stored(), 3u);
    storage.close();
}

// A block removed from the queue is no longer counted.
TEST(StorageWriter, RemovingAQueuedBlockUncountsIt) {
    auto db = std::make_shared<MemoryDb>();
    csdb::Storage storage;
    ASSERT_TRUE(openAsync(storage, db));
    const auto pools = chain(3);

    db->hold();
    ASSERT_TRUE(storage.pool_save(pools[0]));
    ASSERT_TRUE(db->waitCalls(1));
    ASSERT_TRUE(storage.pool_save(pools[1]));
    ASSERT_TRUE(storage.pool_save(pools[2]));

    std::promise<csdb::Pool> removed;
    auto removedFut = removed.get_future();
    std::thread remover([&] { removed.set_value(storage.pool_remove_last()); });
    std::this_thread::sleep_for(100ms);
    db->release();
    remover.join();

    EXPECT_EQ(removedFut.get().hash(), pools[2].hash());
    EXPECT_EQ(storage.size(), 2u);
    EXPECT_EQ(storage.last_hash(), pools[1].hash());
    ASSERT_TRUE(storage.flush());
    EXPECT_EQ(db->stored(), 2u);
    storage.close();
}

// The repair of the last block while it still waits in the queue removes that block only; the blocks
// before it stay counted and reach the database.
TEST(StorageWriter, RepairOfAQueuedLastBlockKeepsTheBlocksBeforeIt) {
    auto db = std::make_shared<MemoryDb>();
    csdb::Storage storage;
    ASSERT_TRUE(openAsync(storage, db));
    const auto pools = chain(3);

    db->hold();
    ASSERT_TRUE(storage.pool_save(pools[0]));
    ASSERT_TRUE(db->waitCalls(1));
    ASSERT_TRUE(storage.pool_save(pools[1]));
    ASSERT_TRUE(storage.pool_save(pools[2]));

    std::promise<bool> repaired;
    auto repairedFut = repaired.get_future();
    std::thread repairer([&] { repaired.set_value(storage.pool_remove_last_repair(2, pools[2].hash())); });
    std::this_thread::sleep_for(100ms);
    db->release();
    repairer.join();

    EXPECT_TRUE(repairedFut.get());
    EXPECT_EQ(storage.size(), 2u);
    EXPECT_EQ(storage.last_hash(), pools[1].hash());
    ASSERT_TRUE(storage.flush());
    EXPECT_TRUE(db->has(pools[1].hash())) << "a counted block was dropped from the queue";
    EXPECT_FALSE(db->has(pools[2].hash()));
    storage.close();
}

// The database refuses every write (disk full): flush() and pool_save() report it instead of waiting,
// and close() returns, so that the node can save no checkpoint ahead of the disk and can stop.
TEST(StorageWriter, PersistentWriteFailureNeitherHangsNorStops) {
    auto db = std::make_shared<MemoryDb>();
    auto storage = std::make_shared<csdb::Storage>();
    ASSERT_TRUE(openAsync(*storage, db));
    const auto pools = chain(2);

    db->fail(true);
    ASSERT_TRUE(storage->pool_save(pools[0]));
    ASSERT_TRUE(db->waitCalls(1));

    bool flushed = true;
    ASSERT_TRUE(returnsWithin([storage, &flushed] { flushed = storage->flush(); }, 5s)) << "flush() hangs";
    EXPECT_FALSE(flushed);
    bool saved = true;
    ASSERT_TRUE(returnsWithin([storage, p = pools[1], &saved] { saved = storage->pool_save(p); }, 5s)) << "pool_save() hangs";
    EXPECT_FALSE(saved) << "a block was queued while the database refuses writes";
    EXPECT_EQ(storage->size(), 1u);
    ASSERT_TRUE(returnsWithin([storage] { storage->close(); }, 5s)) << "close() hangs";
}

// When the database accepts writes again the queued blocks are written and the storage works on.
TEST(StorageWriter, RecoversWhenWritesSucceedAgain) {
    auto db = std::make_shared<MemoryDb>();
    csdb::Storage storage;
    ASSERT_TRUE(openAsync(storage, db));
    const auto pools = chain(3);

    db->fail(true);
    ASSERT_TRUE(storage.pool_save(pools[0]));
    ASSERT_TRUE(db->waitCalls(1));
    EXPECT_FALSE(storage.flush());
    db->fail(false);

    // the writer retries within a second or so
    bool flushed = false;
    for (int i = 0; i < 50 && !flushed; ++i) {
        std::this_thread::sleep_for(100ms);
        flushed = storage.flush();
    }
    ASSERT_TRUE(flushed);
    EXPECT_TRUE(db->has(pools[0].hash()));
    ASSERT_TRUE(storage.pool_save(pools[1]));
    ASSERT_TRUE(storage.pool_save(pools[2]));
    ASSERT_TRUE(storage.flush());
    EXPECT_EQ(db->stored(), 3u);
    EXPECT_EQ(storage.size(), 3u);
    storage.close();
}

// The only block waiting for a failing database is removed (fork resolution): nothing fails any more,
// so once the disk is freed the next block must be accepted, not refused until a restart.
TEST(StorageWriter, RemovingTheLastQueuedBlockEndsTheFailure) {
    auto db = std::make_shared<MemoryDb>();
    csdb::Storage storage;
    ASSERT_TRUE(openAsync(storage, db));
    const auto pools = chain(2);

    db->fail(true);
    ASSERT_TRUE(storage.pool_save(pools[0]));
    ASSERT_TRUE(db->waitCalls(1));
    EXPECT_FALSE(storage.flush());
    const csdb::Pool removed = storage.pool_remove_last();
    EXPECT_EQ(removed.hash(), pools[0].hash());
    EXPECT_EQ(storage.size(), 0u);
    db->fail(false);

    ASSERT_TRUE(storage.pool_save(pools[0])) << "the storage still refuses writes after the queue emptied";
    ASSERT_TRUE(storage.flush());
    EXPECT_TRUE(db->has(pools[0].hash()));
    storage.close();
}

// A chain reorganisation below the deferred block: the node removes stored blocks and saves the
// competing ones at the same sequences. pool_load(sequence) must then return the new blocks, not the
// removed ones still held by the storage's read cache (the stale block made the next consistency
// check fail for ever, and the node stopped two blocks above the fork point).
namespace {

void checkReplacedBlocksAreLoaded(bool asyncWrites) {
    auto db = std::make_shared<MemoryDb>();
    csdb::Storage storage;
    csdb::Storage::OpenOptions opt;
    opt.db = db;
    opt.writeBatchSize = 100;
    opt.asyncWrites = asyncWrites;
    ASSERT_TRUE(storage.open(opt));

    const auto own = chain(5);
    for (const auto& p : own) ASSERT_TRUE(storage.pool_save(p));
    for (cs::Sequence s = 0; s < 5; ++s) ASSERT_EQ(storage.pool_load(s).hash(), own[s].hash());

    // unwind to #2, then the competing #3 and #4
    ASSERT_EQ(storage.pool_remove_last().hash(), own[4].hash());
    ASSERT_EQ(storage.pool_remove_last().hash(), own[3].hash());
    std::vector<csdb::Pool> other;
    csdb::PoolHash prev = own[2].hash();
    for (cs::Sequence s = 3; s < 5; ++s) {
        csdb::Pool p(prev, s);
        p.add_user_field(1, std::string(100, static_cast<char>('z' - s)));
        ASSERT_TRUE(p.compose());
        ASSERT_NE(p.hash(), own[s].hash());
        prev = p.hash();
        other.push_back(p);
        ASSERT_TRUE(storage.pool_save(p));
    }

    EXPECT_EQ(storage.pool_load(3).hash(), other[0].hash());
    EXPECT_EQ(storage.pool_load(4).hash(), other[1].hash());
    EXPECT_EQ(storage.pool_load(4).previous_hash(), storage.pool_load(3).hash());
    EXPECT_FALSE(storage.pool_load(own[3].hash()).is_valid());
    EXPECT_FALSE(storage.pool_load(own[4].hash()).is_valid());
    storage.close();
}

}  // namespace

TEST(StorageWriter, BlocksSavedAfterAnUnwindReplaceTheCachedOnes) {
    checkReplacedBlocksAreLoaded(false);
}

TEST(StorageWriter, BlocksSavedAfterAnUnwindReplaceTheCachedOnesAsync) {
    checkReplacedBlocksAreLoaded(true);
}

// The recovery path of BlockChain::removeLastBlock(): when the removed block cannot be read back, the
// node removes it with pool_remove_last_repair(). That path must forget the block in the read cache
// too, and a node restarted afterwards (the storage rescan fills the cache again) must read the blocks
// saved after the removal, not the removed ones.
namespace {

csdb::Pool competing(const csdb::PoolHash& prev, cs::Sequence seq) {
    csdb::Pool p(prev, seq);
    p.add_user_field(1, std::string(100, static_cast<char>('z' - seq)));
    EXPECT_TRUE(p.compose());
    return p;
}

void checkRepairedBlockIsForgotten(bool asyncWrites) {
    auto db = std::make_shared<MemoryDb>();
    csdb::Storage::OpenOptions opt;
    opt.db = db;
    opt.writeBatchSize = 100;
    opt.asyncWrites = asyncWrites;

    const auto own = chain(4);
    csdb::Pool other;
    {
        csdb::Storage storage;
        ASSERT_TRUE(storage.open(opt));
        for (const auto& p : own) ASSERT_TRUE(storage.pool_save(p));
        ASSERT_TRUE(storage.flush());
        ASSERT_EQ(storage.pool_load(3).hash(), own[3].hash());    // #3 now in the read cache

        ASSERT_TRUE(storage.pool_remove_last_repair(3, own[3].hash()));
        EXPECT_EQ(storage.size(), 3u);
        EXPECT_EQ(storage.last_hash(), own[2].hash());
        EXPECT_FALSE(storage.pool_load(3).is_valid()) << "the repaired block is still read from the cache";
        EXPECT_FALSE(storage.pool_load(own[3].hash()).is_valid());

        other = competing(own[2].hash(), 3);
        ASSERT_NE(other.hash(), own[3].hash());
        ASSERT_TRUE(storage.pool_save(other));
        EXPECT_EQ(storage.pool_load(3).hash(), other.hash());
        ASSERT_TRUE(storage.flush());
        storage.close();
    }

    // restart: the rescan reads every stored block and fills the cache from the database
    csdb::Storage storage;
    ASSERT_TRUE(storage.open(opt));
    EXPECT_EQ(storage.size(), 4u);
    EXPECT_EQ(storage.last_hash(), other.hash());
    EXPECT_EQ(storage.pool_load(3).hash(), other.hash());
    EXPECT_EQ(storage.pool_load(other.hash()).sequence(), 3u);
    EXPECT_FALSE(storage.pool_load(own[3].hash()).is_valid());
    storage.close();
}

}  // namespace

TEST(StorageWriter, RepairedLastBlockIsForgottenAndRestartReadsItsReplacement) {
    checkRepairedBlockIsForgotten(false);
}

TEST(StorageWriter, RepairedLastBlockIsForgottenAndRestartReadsItsReplacementAsync) {
    checkRepairedBlockIsForgotten(true);
}

// --resolve-fork cuts the chain through the --set-bc-top path, but only when the stored block at the
// fork point is the recorded one: then every block above it goes and the fork point stays the last
// block; otherwise nothing is removed and the open fails with TruncateMismatch.
TEST(StorageWriter, ResolveForkTruncatesExactlyAtTheRecordedForkPoint) {
    auto db = std::make_shared<MemoryDb>();
    const auto own = chain(10);
    {
        csdb::Storage storage;
        csdb::Storage::OpenOptions opt;
        opt.db = db;
        ASSERT_TRUE(storage.open(opt));
        for (const auto& p : own) ASSERT_TRUE(storage.pool_save(p));
        ASSERT_TRUE(storage.flush());
        storage.close();
    }
    ASSERT_EQ(db->stored(), 10u);

    // a record that no longer matches (another hash at #6): refused, nothing removed
    {
        csdb::Storage storage;
        storage.set_truncate_check(own[5].hash());
        csdb::Storage::OpenOptions opt;
        opt.db = db;
        opt.newBlockchainTop = 6;
        EXPECT_FALSE(storage.open(opt));
        EXPECT_EQ(storage.last_error(), csdb::Storage::TruncateMismatch);
        EXPECT_EQ(db->stored(), 10u);
    }
    // a fork point that is not stored at all: refused too
    {
        csdb::Storage storage;
        storage.set_truncate_check(own[6].hash());
        csdb::Storage::OpenOptions opt;
        opt.db = db;
        opt.newBlockchainTop = 12;
        EXPECT_FALSE(storage.open(opt));
        EXPECT_EQ(storage.last_error(), csdb::Storage::TruncateMismatch);
        EXPECT_EQ(db->stored(), 10u);
    }
    // the recorded block: everything above #6 goes, #6 stays
    {
        csdb::Storage storage;
        storage.set_truncate_check(own[6].hash());
        csdb::Storage::OpenOptions opt;
        opt.db = db;
        opt.newBlockchainTop = 6;
        ASSERT_TRUE(storage.open(opt));
        storage.close();
    }
    EXPECT_EQ(db->stored(), 7u);
    for (cs::Sequence s = 0; s <= 6; ++s) EXPECT_TRUE(db->has(own[s].hash())) << s;
    for (cs::Sequence s = 7; s < 10; ++s) EXPECT_FALSE(db->has(own[s].hash())) << s;

    // and the storage opens normally afterwards, ending at the fork point
    csdb::Storage storage;
    csdb::Storage::OpenOptions opt;
    opt.db = db;
    ASSERT_TRUE(storage.open(opt));
    EXPECT_EQ(storage.size(), 7u);
    EXPECT_EQ(storage.last_hash(), own[6].hash());
    storage.close();
}
