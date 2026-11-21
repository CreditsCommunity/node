#ifndef BLOCKCHAIN_HPP
#define BLOCKCHAIN_HPP

#include <chrono>
#include <limits>
#include <functional>
#include <list>
#include <optional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <fstream>
#include <iostream>
#include <string>

#include <csdb/address.hpp>
#include <csdb/amount.hpp>
#include <csdb/amount_commission.hpp>
#include <csdb/pool.hpp>
#include <csdb/storage.hpp>
#include <csdb/internal/types.hpp>

#include <csnode/nodecore.hpp>
#include <csnode/multiwallets.hpp>
#include <csnode/walletsids.hpp>
#include <csnode/poolcache.hpp>
#include <csnode/caches_serialization_manager.hpp>
#include <csnode/forkguardstate.hpp>

#include <roundpackage.hpp>

#include <lib/system/concurrent.hpp>

#include <condition_variable>
#include <mutex>

#include <lib/system/common.hpp>

namespace cs {
class BlockHashes;
class WalletsIds;
class Fee;
class TransactionsIndex;
class TransactionsPacket;
class BlockChain_Serializer;

/** @brief   The synchronized block signal emits when block is trying to be stored */
using TryToStoreBlockSignal = cs::Signal<void(const csdb::Pool&, bool*)>;
using NeedStopSignal = cs::Signal<void(bool)>;
/** @brief   The new block signal emits when finalizeBlock() occurs just before recordBlock() */
using StoreBlockSignal = cs::Signal<void(const csdb::Pool&)>;
using OrderNecessaryBlockSignal = cs::Signal<void(csdb::PoolHash, cs::Sequence)>;

/** @brief   The write block or remove block signal emits when block is flushed to disk */
using ChangeBlockSignal = cs::Signal<void(const cs::Sequence)>;
using RemoveBlockSignal = cs::Signal<void(const csdb::Pool&)>;
using AlarmSignal = cs::Signal<void(const cs::Sequence)>;
using ReadBlockSignal = csdb::ReadBlockSignal;
using StartReadingBlocksSignal = csdb::BlockReadingStartedSingal;
using StopReadingBlocksSignal = cs::Signal<void(uint64_t, bool)>;
using SuccessQSSignal = cs::Signal<void(csdb::Amount, csdb::Amount, bool, bool, uint32_t)> ;
}  // namespace cs

class BlockChain {
public:
    using Transactions = std::vector<csdb::Transaction>;
    using WalletId = csdb::internal::WalletId;
    using WalletAddress = csdb::Address;
    using WalletData = cs::WalletsCache::WalletData;

    enum class AddressType {
        PublicKey,
        Id
    };

    explicit BlockChain(csdb::Address genesisAddress, csdb::Address startAddress,
                        bool recreateIndex = false);
    ~BlockChain();

    bool init(
      const std::string& path,
      cs::CachesSerializationManager*,
      std::set<cs::PublicKey>& initialConfidants,
      cs::Sequence newBlockchainTop = cs::kWrongSequence
    );
    // called immediately after object construction, better place to subscribe on signals
    void subscribeToSignals();

    bool isGood() const;

    void flushIndexes();

    // false while trxIndex has an unwalked floor gap; consensus uses this to gate Trusted.
    bool isTrxIndexReady() const;

    // pruned storage ([storage] prune_keep_blocks): the blocks before getFirstStoredSequence() were
    // removed once a later periodic checkpoint made them unnecessary
    cs::Sequence getFirstStoredSequence() const;
    // the stored bytes of a block, under the storage lock, for a snapshot: no parsing, no pool cache
    bool blockBytes(cs::Sequence sequence, cs::Bytes& out) const;
    // pruned storage: the blocks below this one are being or will be removed (0: nothing scheduled)
    cs::Sequence pruneTarget() const;

    // Backward sync (docs/specs/2026-10-17-backward-sync-design.md): the block below the first
    // stored one must have the hash prevHash. first is 0 when the storage holds the chain from block
    // 0; prevHash is empty then, and when the first stored block cannot be read
    struct BackfillAnchor {
        cs::Sequence first = 0;
        csdb::PoolHash prevHash;
    };
    BackfillAnchor getBackfillAnchor() const;
    // stores a verified block first - 1 without applying it to the caches: transaction index, block
    // hashes, non-empty blocks, storage, then the first stored sequence, in that order. false if it
    // was not stored; the index and the block hashes may then hold it ahead of the storage, which
    // the next attempt and the start-up repair both take as it is
    bool storeBackfilledBlock(const csdb::Pool& pool);
    // the backfilled blocks to disk: the index, the block hashes, then the storage, so that after a
    // power cut the first two are not behind the storage
    bool flushBackfill();

    // return unique id of database if at least one unique block has written, otherwise (only genesis block) 0
    uint64_t uuid() const;

    // utility methods

    csdb::Address getAddressByType(const csdb::Address& addr, AddressType type) const;
    bool isEqual(const csdb::Address& laddr, const csdb::Address& raddr) const;

    static csdb::Address getAddressFromKey(const std::string&);
    static csdb::Address getAddressFromKey(const cs::Bytes&);

    static uint64_t getBlockTime(const csdb::Pool& block) noexcept;
    static std::string poolInfo(const csdb::Pool& pool);
    // create/save block and related methods

    /**
     * @fn    bool BlockChain::storeBlock(csdb::Pool pool, bool by_sync);
     *
     * @brief Stores a block
     *
     * @author    Alexander Avramenko
     * @date  23.11.2018
     *
     * @param pool    The pool representing block to store in blockchain. Its sequence number MUST be
     *                set. It will be modified.
     * @param by_sync False if block is new, just constructed, true if block is received via sync subsystem.
     *                False - addNewWalletsToPool() called. If true updateWalletIds() called.
     *
     * @return    True if it succeeds, false if it fails. True DOES NOT MEAN the block recorded to
     *            chain. It means block is correct and possibly recorded. If it is not recorded now, it is cached
     *            for future use and will be recorded on time
     */

    bool storeBlock(csdb::Pool& pool, cs::PoolStoreType type);

    /**
     * @fn    std::optional<csdb::Pool> BlockChain::createBlock(csdb::Pool pool);
     *
     * @brief Creates a block and records to blockchain
     *
     * @author    Alexander Avramenko
     * @date  23.11.2018
     *
     * @param pool    The pool.
     *
     * @return    The new recorded block if ok, otherwise nullopt.
     */

    std::optional<csdb::Pool> createBlock(csdb::Pool& pool) {
        return recordBlock(pool, true);
    }

    void removeWalletsInPoolFromCache(const csdb::Pool& pool);
    void removeLastBlock();
    void showDBParams();
    /**
     * Mark last block as compromised and handle the situation:
     *  - store required parameters  
     *  - make a request for proper block variant
     *
     * @author  Alexander Avramenko
     * @date    31.01.2020
     *
     * @param   desired_hash    The desired hash of last block to request.
     *
     * @returns True if it succeeds, false if it fails.
     */

    bool compromiseLastBlock(const csdb::PoolHash& desired_hash);

    // updates fees in every transaction
    void setTransactionsFees(cs::TransactionsPacket& packet);
    void setTransactionsFees(csdb::Pool& pool, cs::PoolStoreType type = cs::PoolStoreType::Created);
    void setTransactionsFees(std::vector<csdb::Transaction>& transactions);
    void setTransactionsFees(std::vector<csdb::Transaction>& transactions, const cs::Bytes& characteristicMask);

    bool addNewWalletsToPool(csdb::Pool& pool);
    void updateLastTransactions(const std::vector<std::pair<cs::PublicKey, csdb::TransactionID>>&);

    bool checkForConsistency(csdb::Pool & pool, bool isNew);

    // storage adaptor
    void close();
    void requestStop() { stop_ = true; }
    // a node that starts on an empty database and imports a snapshot (Node's bootstrap mode, before
    // init): the importer writes caches/hashdb and caches/seqdb itself and refuses to run while they
    // exist, so the empty ones the constructor opened are closed and removed first, and opened again
    // after it. Called while nothing reads the block hashes (the chain is not open)
    void releaseHashDbs();
    void reopenHashDbs();
    // init() failed because the quick-start checkpoint it loaded continues after the last block on
    // disk: that checkpoint cannot be used with this database
    bool checkpointAheadOfBlocks() const { return checkpointAheadOfBlocks_; }
    bool getTransaction(const csdb::Address& addr, const int64_t& innerId, csdb::Transaction& result) const;
    void arrangeBlocksInCache();


public:
    std::string getLastTimeStamp() const;
    cs::Bytes getLastRealTrusted() const;
    bool updateLastBlock(cs::RoundPackage& rPackage);
    bool updateLastBlock(cs::RoundPackage& rPackage, const csdb::Pool& poolFrom);
    bool deferredBlockExchange(cs::RoundPackage& rPackage, const csdb::Pool& newPool);
    // the writer's block from this node's block of the same sequence and the round package's meta
    static csdb::Pool blockFromPackageMeta(const csdb::Pool& from, const cs::PoolMetaInfo& meta);
    // a block of this sequence is deferred (not yet on disk), so it can still be replaced
    bool hasDeferredBlock(cs::Sequence sequence) const;
    // the round package's pool signatures sign the writer's block built from the deferred block of
    // that sequence, checked against the deferred block's confidants (no state change)
    bool packageBlockVerifies(const cs::RoundPackage& rPackage) const;
    // the node adopted the writer's round package for the block of this sequence: updateLastBlock may
    // take the writer's block (once)
    void allowTakeover(cs::Sequence sequence);
    bool isSpecial(const csdb::Transaction& t);
    cs::Bytes checkForSpecialTransactions(const std::vector<csdb::Transaction>& trxs, cs::Sequence seq);
    cs::Sequence getLastSeq() const;

    static inline const csdb::user_field_id_t kFieldTimestamp = 0;
    static inline const csdb::user_field_id_t kFieldServiceInfo = 1;
    static inline const csdb::user_field_id_t kFieldBlockReward = 3;

    static void setTimestamp(csdb::Pool& block, const std::string& timestamp);
    static void setBootstrap(csdb::Pool& block, bool is_bootstrap);
    static bool isBootstrap(const csdb::Pool& block);

    // Longest-chain guard. It is called from inside removeLastBlock() and compromiseLastBlock(), so
    // every automatic rollback of the node's own last block passes through it, whatever the caller
    // (the sync fork path, the uncertain-replacement path, the antifork cache and the bad-hash reply).
    // It returns true while the resulting head would stay at or above rollbackHighWater_ -
    // max_auto_rollback, where rollbackHighWater_ is the head captured at the start of the current
    // rollback episode (the first automatic removal from any path) and kept in reorg-episode.json next
    // to the database, so a restart does not give a fresh full depth. Once the limit would be crossed
    // it records the fork (fork-held.json), logs a prominent alert and returns false: the chain stays
    // intact, the node reports FORK HELD and the operator decides (--resolve-fork, --set-bc-top). The
    // state clears when the episode ends (the head climbs above the reference) or the operator
    // truncates. An operator accepts a deliberate deep switch with [params] allow_deep_rollback or the
    // CS_ALLOW_DEEP_ROLLBACK environment variable, for one episode. Local only: it changes how far
    // this node unwinds, never block content.
    bool guardAutoRollback();

    // True if the competing block carries round confirmations signed by the confidants the node
    // already trusts - the confidants of its own current last block - exactly as BlockVerifier checks
    // them. A non-bootstrap competing block that fails this check never triggers a rollback: it is not
    // vouched for by any authority this node recognises (a self-signed block vouches only for itself).
    // A bootstrap block carries no such confirmations and always fails here (BlockVerifier accepts it
    // without them); competingBlockMayTriggerUnwind() sends it to isBootstrapAuthenticatedLocally().
    // Strict for the unwind decision: a confirmation mask of more than one entry, of the same size as our
    // last block's confidants, with as many signatures as it marks, and a majority of them confirming.
    bool hasTrustedConfirmations(const csdb::Pool& competing) const;

    // True if a bootstrap-flagged competing block is authenticated locally: its own signatures verify
    // against its confidants AND those confidants are exactly the confidants of a round table this node
    // itself accepted (the current round or the two before it). A bootstrap flag alone is a bit anyone
    // can set, so an unauthenticated bootstrap block never triggers an automatic unwind.
    bool isBootstrapAuthenticatedLocally(const csdb::Pool& competing) const;

    // The single gate for an automatic unwind in response to a competing block: the operator override,
    // or a non-bootstrap block with trusted confirmations, or a locally authenticated bootstrap block.
    // A block that passes is still subject to the depth bound inside removeLastBlock().
    bool competingBlockMayTriggerUnwind(const csdb::Pool& competing) const;

    // The operator has approved a deliberate deep switch: [params] allow_deep_rollback=true or the
    // CS_ALLOW_DEEP_ROLLBACK environment variable, until one episode has completed with it (then it is
    // off for the rest of the run). It lifts both the authority gate and the depth bound. (--resolve-fork
    // approves nothing as wide: see approvedBranch_.)
    bool deepRollbackApproved() const;
    // CS_ALLOW_DEEP_ROLLBACK is set to a non-empty value
    static bool deepRollbackEnvSet();

    // The local files of the guard live in `dir` (the folder next to the database). Loads the
    // persisted episode reference and the fork record; an episode the chain has already passed is
    // dropped. An empty dir keeps everything in memory only. Called by init().
    void setGuardStateDir(const std::string& dir);

    // FORK HELD: the node refused to unwind for a competing chain, the episode has not ended and the
    // operator has not dismissed it.
    bool forkHeld() const;
    // the record while FORK HELD, else nothing
    std::optional<cs::forkguard::ForkRecord> forkHeldRecord() const;
    // the record, held or keeping a dismissal
    std::optional<cs::forkguard::ForkRecord> forkRecord() const;

    // The operator keeps this node's chain: FORK HELD ends, the record keeps the competing branch as
    // dismissed (still refused, never reported again; another branch is). False if no fork is held.
    bool dismissFork();
    // the dismiss-fork file next to the database: dismissFork() and remove the file. Called every
    // round, every few seconds from a timer and at start-up; true if a fork was dismissed.
    bool checkDismissTrigger();

    // A refusal for a competing block not vouched for by our confidants becomes FORK HELD only if the
    // head has not moved since, at the second round start after it or a minute later (whichever is
    // first); a head that moves drops it. Called at every round start (roundStarted) and from a timer.
    // True if FORK HELD was raised now.
    bool promotePendingHold(bool roundStarted);
    bool forkHoldPending() const {
        return pendingHold_.has_value();
    }

    // the sync reply being stored comes from this peer, which announced this height (for the fork
    // record); cleared with clearSyncSource()
    void setSyncSource(const cs::PublicKey& key, cs::Sequence announcedHeight);
    void clearSyncSource();
    // resolves a peer key to "ip:port" for the fork record (set by the node; may return "")
    void setPeerAddressResolver(std::function<std::string(const cs::PublicKey&)> resolver);

    // --resolve-fork: init() truncates to `seq` only if the stored block there has `hash`, then clears
    // the fork record. When the record's fork point was exact, it also records the competing block
    // right after it (`competingHash` at seq + 1) as the one approved branch (resolve-approved.json).
    void setResolveFork(cs::Sequence seq, const csdb::PoolHash& hash, const std::string& competingHash = std::string{});
    // the branch approved with --resolve-fork, while it is pending
    std::optional<cs::forkguard::ApprovedBranch> approvedBranch() const {
        return approvedBranch_;
    }
    // init() refused to truncate: the stored block at the requested sequence has another hash
    bool truncationRefused() const {
        return truncationRefused_;
    }

#ifdef TESTING
    // test-only accessors for the longest-chain guard
    void test_setLastSequence(cs::Sequence s) { lastSequence_ = s; }
    cs::Sequence test_rollbackHighWater() const { return rollbackHighWater_; }
    bool test_rollbackEpisode() const { return rollbackEpisode_; }
    void test_resetRollbackEpisode() {
        rollbackEpisode_ = false; rollbackHighWater_ = 0; approvedBranch_.reset(); deepOverrideSpent_ = false; pendingHold_.reset();
        std::lock_guard l(guardMutex_); forkRecord_.reset();
    }
    // make `block` the node's last block (the deferred one), as getLastBlock() returns it
    void test_setLastBlock(const csdb::Pool& block) { deferredBlock_ = block; lastSequence_ = block.sequence(); }
    // let the episode end as if a block above the reference had been recorded
    void test_onHeadAdvanced() { onHeadAdvanced(); checkApprovedBranch(); }
    bool test_recordForkHeld(const char* reason) { return recordForkHeld(reason); }
    uint64_t test_onRefusedBlock(std::chrono::steady_clock::time_point now) {
        std::lock_guard l(guardMutex_); return refusedLine_.onEvent(now);
    }
    void test_setSelectionUnfinished(const csdb::PoolHash& lastPrev) { selectionFinished_ = false; lastPrevHash_ = lastPrev; antiForkMode_ = true; emittingRequest_ = 1; }
    bool test_selectionFinished() const { return selectionFinished_; }
    bool test_antiForkMode() const { return antiForkMode_; }
    uint8_t test_emittingRequest() const { return emittingRequest_; }
    csdb::PoolHash test_lastPrevHash() const { return lastPrevHash_; }
#endif

    const cs::MultiWallets& multiWallets() const;

    /**
     * @fn    std::size_t BlockChain::getCachedBlocksSize() const;
     *
     * @brief Gets amount of cached blocks
     *
     * @author    Alexander Avramenko
     * @date  06.12.2018
     *
     * @return    The cached blocks amount.
     */

    std::size_t getCachedBlocksSize() const;
    std::size_t getCachedBlocksSizeSynced() const;
    void clearBlockCache();

    void cacheLastBlocks();
    void replaceCachedIncorrectBlock(const csdb::Pool& block);
    void getCachedMissedBlock(const csdb::Pool& block);
    void badBlockIssue(const csdb::Pool& pool);
    void lookForBadBlocks();

    std::vector<cs::Sequence>* getIncorrectBlockNumbers();
    // continuous interval from ... to
    using SequenceInterval = std::pair<cs::Sequence, cs::Sequence>;

    /**
     * @fn    std::vector<SequenceInterval> BlockChain::getReqiredBlocks() const;
     *
     * @brief Gets required blocks in form vector of intervals. Starts with last written block and view through all cached
     * ones. Each interval means [first..second] including bounds. Last interval ends with current round number
     *
     * @author    Alexander Avramenko
     * @date  23.11.2018
     *
     * @return    The required blocks in form vector of intervals
     */

    std::vector<SequenceInterval> getRequiredBlocks() const;

    /**
     * @fn    void BlockChain::testCachedBlocks();
     *
     * @brief Tests cached blocks: removes outdated, records actual until sequence interrupted
     *
     * @author    Alexander Avramenko
     * @date  23.11.2018
     */

    void testCachedBlocks();
    std::optional<SequenceInterval> getFreeSpaceBlocks() const;

    bool isLastBlockUncertain() const {
        return uncertainLastBlockFlag_;
    }

public signals:
    //orderNecessaryBlock(&tryBlock, lastBlock.sequence());
    cs::OrderNecessaryBlockSignal orderNecessaryBlock;

    /** @brief The new block event. Raised when the next incoming block is finalized and just before stored into chain */
    cs::StoreBlockSignal storeBlockEvent;
    // the deferred block was replaced by the writer's block of the same sequence (not stored again)
    cs::StoreBlockSignal lastBlockReplacedEvent;

    cs::NeedStopSignal stopNode;

    /** @brief The event storing synchronized block. Raised when the next incoming block is trying to be stored into chain */
    cs::TryToStoreBlockSignal tryToStoreBlockEvent;

    /** @brief The cached block event. Raised when the next block is flushed to storage */
    cs::ChangeBlockSignal cachedBlockEvent;

    /** @brief The remove block event. Raised when the next block is flushed to storage */
    cs::RemoveBlockSignal removeBlockEvent;

    /** @brief Alarm event. Block Isn't correct */
    cs::AlarmSignal alarmBadBlock;

    /** @brief Alarm event. Uncertain that last block is valid */
    cs::AlarmSignal uncertainBlock;

    const cs::ReadBlockSignal& readBlockEvent() const;
    const cs::StartReadingBlocksSignal& startReadingBlocksEvent() const;

    cs::StopReadingBlocksSignal stopReadingBlocksEvent;
    cs::SuccessQSSignal successfullQuickStartEvent;
    // a periodic checkpoint of this block was saved (live), with its state digest
    cs::Signal<void(cs::Sequence)> checkpointSavedEvent;

public slots:

    // subscription is placed in SmartContracts constructor
    void onPayableContractReplenish(const csdb::Transaction& starter) {
        this->walletsCacheUpdater_->invokeReplenishPayableContract(starter, false /*inverse*/);
    }
    void onContractTimeout(const csdb::Transaction& starter) {
        this->walletsCacheUpdater_->rollbackExceededTimeoutContract(starter, csdb::Amount(0), false /*inverse*/);
    }
    void onContractEmittedAccepted(const csdb::Transaction& emitted, const csdb::Transaction& starter) {
        this->walletsCacheUpdater_->smartSourceTransactionReleased(emitted, starter, false /*inverse*/);
    }
    void rollbackPayableContractReplenish(const csdb::Transaction& starter) {
        this->walletsCacheUpdater_->invokeReplenishPayableContract(starter, true /*inverse*/);
    }
    void rollbackContractTimeout(const csdb::Transaction& starter) {
        this->walletsCacheUpdater_->rollbackExceededTimeoutContract(starter, csdb::Amount(0), true /*inverse*/);
    }
    void rollbackContractEmittedAccepted(const csdb::Transaction& emitted, const csdb::Transaction& starter) {
        this->walletsCacheUpdater_->smartSourceTransactionReleased(emitted, starter, true /*inverse*/);
    }

public:

    // load methods

    csdb::Pool loadBlock(const csdb::PoolHash&) const;
    csdb::Pool loadBlock(const cs::Sequence sequence) const;
    csdb::Pool loadBlockForSync(const cs::Sequence sequence) const;
    csdb::Pool loadBlockMeta(const csdb::PoolHash&, size_t& cnt) const;
    csdb::Transaction loadTransaction(const csdb::TransactionID&) const;
    void iterateOverWallets(const std::function<bool(const cs::PublicKey&, const cs::WalletsCache::WalletData&)>);
    csdb::Pool getLastBlock() const {
        return loadBlock(getLastSeq());
    }

    bool isAntiForkModeOn() {
        return antiForkMode_;
    }
    // info

    size_t getSize() const;
    uint64_t getWalletsCountWithBalance();
    uint64_t getWalletsCount() const;
    csdb::PoolHash getLastHash() const;
    csdb::PoolHash getHashBySequence(cs::Sequence seq) const;
    cs::Sequence getSequenceByHash(const csdb::PoolHash&) const;

    // get inner data (from caches)

    bool findWalletData(const csdb::Address&, WalletData& wallData, WalletId& id) const;
    bool findWalletData(WalletId id, WalletData& wallData) const;
    bool findWalletData(const csdb::Address&, WalletData& wallData) const;
    bool findWalletId(const WalletAddress& address, WalletId& id) const;
    // wallet transactions: pools cache + db search
    void getTransactions(Transactions& transactions, csdb::Address address, uint64_t offset, uint64_t limit);
    void getTransactionsUntill(Transactions& transactions, csdb::Address address, csdb::TransactionID id, uint16_t flagg);
    void getAccountRegTime(uint64_t& aTime, csdb::Address address);
    void setBlocksToBeRemoved(cs::Sequence number);
    static double getStakingCoefficient(cs::StakingCoefficient coeff);

    std::string printWalletCaches();


#ifdef MONITOR_NODE
    void iterateOverWriters(const std::function<bool(const cs::PublicKey&, const cs::WalletsCache::TrustedData&)>);
    void applyToWallet(const csdb::Address&, const std::function<void(const cs::WalletsCache::WalletData&)>); 
#endif
    uint32_t getTransactionsCount(const csdb::Address&);

    csdb::TransactionID getLastTransaction(const csdb::Address&) const;
    cs::Sequence getPreviousPoolSeq(const csdb::Address&, cs::Sequence) const;

    std::pair<cs::Sequence, uint32_t> getLastNonEmptyBlock();
    std::pair<cs::Sequence, uint32_t> getPreviousNonEmptyBlock(cs::Sequence);
    uint64_t getTransactionsCount() const {
        return totalTransactionsCount_;
    }

    const csdb::Address& getGenesisAddress() const;

    bool updateContractData(const csdb::Address& abs_addr, const cs::Bytes& data) const;
    bool getContractData(const csdb::Address& abs_addr, cs::Bytes& data) const;

    const cs::WalletsCache::Updater& getCacheUpdater() const {
        return *(walletsCacheUpdater_.get());
    }

    /**
     * Try to flush deferred block to DB, intended to call on node exit
     *
     * @author  Alexander Avramenko
     * @date    25.09.2019
     */

    void tryFlushDeferredBlock();
    void addIncorrectBlockNumber(cs::Sequence seq);

    // the state the caches hold now: wallets digest, wallet count and consensus settings hash, as a
    // checkpoint saves it (state_digest.bin)
    cs::CheckpointDigest checkpointDigest() const;
    // hash of the consensus settings a checkpoint carries: block reward, mining coefficient, staking
    // and mining flags, minimal stage-1 time, pending settings, DPOS start and the recorded special
    // orders (the initial confidants come from the node's configuration and are not included)
    std::array<uint8_t, 32> consensusSettingsDigest() const;

    csdb::Amount getBlockReward() {
        return csdb::Amount(blockRewardIntegral_, blockRewardFraction_);
    }

    csdb::Amount getMiningCoefficient() {
        return csdb::Amount(miningCoefficientIntegral_, miningCoefficientFraction_);
    }

    bool getStakingOn() {
        return stakingOn_;
    }

    bool getMiningOn() {
        return miningOn_;
    }

    uint32_t getTimeMinStage1() {
        return TimeMinStage1_;
    }

    void setBlockReward(csdb::Amount reward);
    void setMiningCoefficient(csdb::Amount coefficient);
    void setMiningOn(bool mOn);
    void setStakingOn(bool stOn);
    void setTimeMinStage1(uint32_t timeStage1);

    // Consensus-settings change announced by special order 37 but not active yet, and StartingDPOS
    // set by order 9. Node owns this state; it is kept here so that quick-start checkpoints carry it
    // (BlockChain_Serializer), otherwise a node restarted before the activation round never applies it.
    struct PendingConsensusSettings {
        cs::Sequence round = std::numeric_limits<cs::Sequence>::max();  // max: nothing pending
        bool stakingOn = false;
        bool miningOn = false;
        csdb::Amount blockReward{0};
        csdb::Amount miningCoefficient{0};
    };
    void setPendingConsensusSettings(const PendingConsensusSettings& pending);
    PendingConsensusSettings getPendingConsensusSettings() const;
    void setStartingDPOS(cs::Sequence sequence);
    cs::Sequence getStartingDPOS() const;  // 0: never set by order 9

    // Payload of the last special order of each kind whose effect is held only in memory (version
    // limits, consensus limits, MinStakeValue, syncroChangeRound), recorded when a block
    // is applied to the caches, so before a checkpoint of that block is saved. A quick start replays
    // them (Node::restoreSpecialOrders), as a slow start does when it reads the blocks.
    using SpecialOrders = std::map<uint16_t, std::string>;
    void recordSpecialOrders(const csdb::Pool& pool);
    const SpecialOrders& getSpecialOrders() const;

    // compare only state content: transactions, new wallets, sequence, round fee, user fields, trusted mask
    // true if both pools are not valid, or both pools have equal state content
    static bool testContentEqual(const csdb::Pool& lhs, const csdb::Pool& rhs);


private:
    void createCachesPath();
    bool findAddrByWalletId(const WalletId id, csdb::Address& addr) const;
    void writeGenesisBlock();

    void logBlockInfo(csdb::Pool& pool);

    // Thread unsafe
    bool finalizeBlock(csdb::Pool& pool, bool isTrusted, cs::PublicKeys lastConfidants);
    bool applyBlockToCaches(const csdb::Pool& pool);

    void onStartReadFromDB(cs::Sequence lastWrittenPoolSeq);
    void onReadFromDB(csdb::Pool block, bool* shouldStop);
    bool postInitFromDB(bool successfulQuickStart);

    bool updateWalletIds(const csdb::Pool& pool, cs::WalletsCache::Updater& updater);
    bool insertNewWalletId(const csdb::Address& newWallAddress, WalletId newWalletId, cs::WalletsCache::Updater& updater);

    bool findWalletData_Unsafe(WalletId id, WalletData& wallData) const;

    class TransactionsLoader;

    void updateNonEmptyBlocks(const csdb::Pool&);
    // backward sync: a backfilled block with transactions joins the chain of non-empty blocks
    void mergeNonEmptyBlock(const csdb::Pool&);

    bool good_;

    mutable std::recursive_mutex dbLock_;
    csdb::Storage storage_;

    std::unique_ptr<cs::BlockHashes> blockHashes_;
    std::unique_ptr<cs::TransactionsIndex> trxIndex_;

    const csdb::Address genesisAddress_;
    const csdb::Address startAddress_;

    std::unique_ptr<cs::WalletsIds> walletIds_;
    std::unique_ptr<cs::WalletsCache> walletsCacheStorage_;
    std::unique_ptr<cs::WalletsCache::Updater> walletsCacheUpdater_;

    mutable cs::SpinLock cacheMutex_{ATOMIC_FLAG_INIT};

    uint64_t totalTransactionsCount_ = 0;

    struct NonEmptyBlockData {
        cs::Sequence poolSeq;
        uint32_t transCount = 0;
    };

    std::map<cs::Sequence, NonEmptyBlockData> previousNonEmpty_;
    NonEmptyBlockData lastNonEmptyBlock_;

    /**
     * @fn    std::optional<csdb::Pool> BlockChain::recordBlock(csdb::Pool pool, std::optional<cs::PrivateKey> writer_key);
     *
     * @brief Finish pool, sign it or test signature, then record block to chain
     *
     * @author    Alexander Avramenko
     * @date  23.11.2018
     *
     * @param pool    The pool to finish &amp; record to chain.
     *
     * @return    A std::pair of bool (success or fail) and std::optional&lt;csdb::Pool&gt; (recorded
     *            pool)
     */

    std::optional<csdb::Pool> recordBlock(csdb::Pool& pool, bool isTrusted);

    // to store outrunning blocks until the time to insert comes;
    // stores pairs of <sequence, metadata>
    struct BlockMeta {
        csdb::Pool pool;
        // indicates that block has got by sync, so it is checked & tested in other way than ordinary ones
        cs::PoolStoreType type;
    };

    mutable std::mutex cachedBlocksMutex_;
    std::unique_ptr<cs::PoolCache> cachedBlocks_;

    // block storage to defer storing it in blockchain until confirmation from other nodes got
    // (idea is it is more easy not to store block immediately then to revert it after storing)
    csdb::Pool deferredBlock_;

    uint64_t uuidFromHash(const csdb::PoolHash& h) const {
        if (!h.is_empty()) {
            return *reinterpret_cast<uint64_t*>(h.to_binary().data());
        }
        return 0;
    }

    uint64_t uuidFromBlock(const csdb::Pool& block) const {
        if (block.is_valid()) {
            return uuidFromHash(block.hash());
        }
        return 0;
    }

    // may be modified once in uuid() method:
    mutable std::atomic<uint64_t> uuid_ = 0;
    std::atomic<cs::Sequence> lastSequence_;
    cs::Sequence blocksToBeRemoved_ = 0;
    // longest-chain guard: a fixed reference for the current automatic-rollback episode. When the
    // first automatic rollback of an episode starts, rollbackHighWater_ records the head we had then;
    // the node then refuses to unwind automatically below rollbackHighWater_ - MaxAutoRollbackDepth.
    // The reference is cleared only once the chain climbs back strictly above rollbackHighWater_, so
    // alternating removals and appends cannot creep past the limit. Purely local bookkeeping; it
    // never changes any block content.
    bool rollbackEpisode_ = false;
    cs::Sequence rollbackHighWater_ = 0;
    cs::Sequence rollbackLowWater_ = 0;  // lowest head reached in the current episode (for the log)
    // longest-chain guard: where its files live (empty: memory only), the fork it refused (FORK
    // HELD), the --resolve-fork / allow_deep_rollback approvals, the integrity repair in progress
    std::string guardStateDir_;
    mutable std::mutex guardMutex_;
    std::optional<cs::forkguard::ForkRecord> forkRecord_;
    std::chrono::steady_clock::time_point lastDismissedLog_{};  // dismissed branch offered again (hourly)
    // refused competing blocks: one line, then at most one a minute with the count since (guardMutex_)
    cs::forkguard::RepeatedLine refusedLine_;
    // --resolve-fork's approved branch (exact fork point only), pending until the node is past it
    std::optional<cs::forkguard::ApprovedBranch> approvedBranch_;
    std::string resolveCompetingHash_;
    // the approved branch is settled: the node is past its block, or another block took that height
    void checkApprovedBranch();
    void dropApprovedBranch(const char* why);
    bool deepOverrideSpent_ = false;      // allow_deep_rollback served one episode: off until restart
    bool deepOverrideUsed_ = false;       // the current episode ran with allow_deep_rollback
    bool integrityRepair_ = false;        // cacheLastBlocks() is removing blocks of incorrectBlocks_
    struct CompetingBlock {
        cs::Sequence seq = 0;
        csdb::PoolHash hash;
        csdb::PoolHash previous;
        bool vouched = false;  // passed competingBlockMayTriggerUnwind()
    };
    std::optional<CompetingBlock> competing_;  // the competing block being handled (for the record)
    std::optional<std::pair<cs::PublicKey, cs::Sequence>> syncSource_;
    std::function<std::string(const cs::PublicKey&)> peerAddressResolver_;
    cs::Sequence resolveForkSeq_ = cs::kWrongSequence;
    csdb::PoolHash resolveForkHash_;
    bool truncationRefused_ = false;
    // starts the episode at the current head if none is running (persisting its reference)
    void beginRollbackEpisode();
    // the chain grew: ends the episode once the head is above its reference
    void onHeadAdvanced();
    // records the refused fork (once per episode) and logs the alert; reason "depth" or "authority".
    // False (and nothing raised) when the competing block belongs to a branch the operator dismissed.
    // An "authority" refusal is only kept pending until promotePendingHold() sees the head stuck.
    bool recordForkHeld(const char* reason);
    cs::forkguard::ForkRecord buildForkRecord(const char* reason);
    // FORK HELD now: episode, record, file, alert
    void holdFork(cs::forkguard::ForkRecord record);
    struct PendingHold {
        cs::forkguard::ForkRecord record;
        cs::Sequence head = 0;
        std::chrono::steady_clock::time_point since;
        unsigned rounds = 0;
    };
    std::optional<PendingHold> pendingHold_;
    // removeLastBlock()/compromiseLastBlock() may go on: an integrity repair, or the guard allows it
    bool mayUnwindLastBlock();
    std::atomic_bool stop_ = false;
    // the quick-start checkpoint of the last init() is ahead of the blocks on disk
    bool checkpointAheadOfBlocks_ = false;
    // a block failed to apply to the caches, which may now hold part of it: no checkpoint may be
    // saved from them and no further block is recorded; the node stops and the next start reads the
    // blocks again from the last checkpoint
    std::atomic_bool cachesTainted_ = false;
    std::atomic<cs::Sequence> takeoverAllowed_ = cs::kWrongSequence;
    // saves deferredBlock_; an exception of the storage (an engine that needs recovery) stops the node
    bool saveDeferredBlock();

    std::vector<cs::Sequence> incorrectBlocks_;//blocks with wrong hashes (during db reading)
    std::vector<cs::Sequence> badBlocks_;//blocks containing invalid transactions
    bool selectionFinished_ = true;
    csdb::PoolHash lastPrevHash_ = csdb::PoolHash();
    bool antiForkMode_ = false;
    uint8_t emittingRequest_ = 0;
    cs::Sequence neededCacheSeq_ = 0ULL;
    cs::Sequence startingBchSeq_ = 0ULL;


    // support the ability to replace last deferred block by the alternative with the same content, anti-fork feature
    // flag the last block is uncertain:
    bool uncertainLastBlockFlag_ = false;
    // sequence of uncertain block, if uncertainLastBlock_ == true
    cs::Sequence uncertainSequence_ = 0;
    // hash of uncertain block
    csdb::PoolHash uncertainHash_;
    // desired hash of block with the same (uncertain) sequence and the same content as the current (uncertain) block:
    csdb::PoolHash desiredHash_;
    // counter of successfully replaced uncertain blocks
    size_t cntUncertainReplaced = 0;

    void resetUncertainState() {
        uncertainLastBlockFlag_ = false;
        uncertainSequence_ = 0;
        uncertainHash_ = csdb::PoolHash{};
        desiredHash_ = csdb::PoolHash{};
    }

    bool tryQuickStart(cs::CachesSerializationManager*, std::set<cs::PublicKey>& initialConfidants);
    bool bindSerializationManToCaches(cs::CachesSerializationManager*, std::set<cs::PublicKey>& initialConfidants);

    cs::CachesSerializationManager* serializationManPtr_ = nullptr;

    friend class cs::BlockChain_Serializer;

    static constexpr size_t kQuickStartSaveCachesInterval = 500'000;
    std::chrono::steady_clock::time_point lastCheckpointWallClock_ = std::chrono::steady_clock::now();

    void saveLiveCheckpoint(const csdb::Pool& pool);
    // pruned storage: after the periodic checkpoint C, the blocks before C - checkpoint_every -
    // prune_keep_blocks go; they are removed a few at a time as new blocks are applied
    void schedulePruning(cs::Sequence checkpointSeq);
    void pruneStep();
    std::atomic<cs::Sequence> firstStoredSeq_ = 0;
    cs::Sequence pruneUpTo_ = 0;
    // backward sync: the anchor of the first stored block, kept while that block stays the first
    // (under dbLock_)
    BackfillAnchor backfillAnchorLocked() const;
    mutable BackfillAnchor backfillAnchor_;
    // consensus settings persisted in quick-start caches; same defaults as BlockChain_Serializer::clear()
    int32_t blockRewardIntegral_ = 0;
    uint64_t blockRewardFraction_ = 0;
    int32_t miningCoefficientIntegral_ = 0;
    uint64_t miningCoefficientFraction_ = 0;
    bool stakingOn_ = false;
    bool miningOn_ = false;
    uint32_t TimeMinStage1_ = 500;
    PendingConsensusSettings pendingConsensusSettings_;
    cs::Sequence startingDPOS_ = 0;
    SpecialOrders specialOrders_;
};
#endif  //  BLOCKCHAIN_HPP
