#ifndef POOLSYNCHRONIZER_HPP
#define POOLSYNCHRONIZER_HPP

#include <algorithm>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include <csdb/pool.hpp>

#include <csnode/blockchain.hpp>
#include <csnode/nodecore.hpp>
#include <csnode/syncropacing.hpp>

#include <lib/system/common.hpp>
#include <lib/system/timer.hpp>
#include <lib/system/signals.hpp>

#include <net/neighbourhood.hpp>

namespace cs {
// The peers the sync asks first after --resolve-fork (those that have the block the operator chose).
// Their order comes from what peers claim (version, height), so it only biases the choice and never
// pins it: a random one of the usable preferred sources is asked; a source asked that brings no
// progress of the chain within `timeout` is demoted for the rest of the preference; once every source
// is demoted (or none is usable) the usual random choice applies. So the preference costs at most a
// few timeouts before the node syncs as it would have without it.
class PreferredSources {
public:
    using Clock = std::chrono::steady_clock;
    explicit PreferredSources(Clock::duration timeout = std::chrono::seconds(20))
    : timeout_(timeout) {
    }
    // The sources are fixed when the preference starts (the first non-empty list) and only grow while
    // there is room (at most kMax, never reordered): the node rebuilds its list every few seconds from
    // what peers claim, and a new order must not restart a source's window. An empty list ends the
    // preference (and forgets the demoted ones).
    static constexpr size_t kMax = 3;
    void set(std::vector<PublicKey> sources) {
        if (sources.empty()) {
            sources_.clear();
            demoted_.clear();
            target_.reset();
            return;
        }
        for (const auto& key : sources) {
            if (sources_.size() >= kMax) {
                break;
            }
            if (std::find(sources_.begin(), sources_.end(), key) == sources_.end()) {
                sources_.push_back(key);
            }
        }
    }
    // a reply of `sender` was handled: from the source asked, a reply that did not move our head
    // demotes it at once (it answered, but not with the blocks we need)
    void onReply(const PublicKey& sender, bool progressed) {
        if (target_.has_value() && *target_ == sender && !progressed) {
            demoted_.push_back(sender);
            target_.reset();
        }
    }
    const std::vector<PublicKey>& sources() const {
        return sources_;
    }
    bool demoted(const PublicKey& key) const {
        return std::find(demoted_.begin(), demoted_.end(), key) != demoted_.end();
    }
    // the source to ask now, or none (the usual random choice). usable(key): a neighbour announcing
    // blocks above ours; lastSeq: our last block (its moves are the progress); random(n): 0..n-1
    std::optional<PublicKey> pick(const std::function<bool(const PublicKey&)>& usable, Sequence lastSeq, Clock::time_point now,
                                  const std::function<size_t(size_t)>& random) {
        if (!progressSeen_ || lastSeq != progressSeq_) {
            progressSeen_ = true;
            progressSeq_ = lastSeq;
            progressAt_ = now;
        }
        if (target_.has_value() && now - progressAt_ >= timeout_) {
            // asked, and no block since: not a source to count on
            demoted_.push_back(*target_);
            target_.reset();
            progressAt_ = now;
        }
        std::vector<PublicKey> candidates;
        for (const auto& key : sources_) {
            if (!demoted(key) && usable(key)) {
                candidates.push_back(key);
            }
        }
        if (candidates.empty()) {
            target_.reset();
            return std::nullopt;
        }
        if (target_.has_value() && std::find(candidates.begin(), candidates.end(), *target_) != candidates.end()) {
            return target_;  // the source asked keeps its window until it brings blocks or times out
        }
        progressAt_ = now;  // the window of a newly chosen source starts now
        target_ = candidates[candidates.size() == 1 ? 0 : random(candidates.size()) % candidates.size()];
        return target_;
    }

private:
    Clock::duration timeout_;
    std::vector<PublicKey> sources_;
    std::vector<PublicKey> demoted_;
    std::optional<PublicKey> target_;
    bool progressSeen_ = false;
    Sequence progressSeq_ = 0;
    Clock::time_point progressAt_{};
};

using PoolSynchronizerRequestSignal = Signal<void(const PublicKey& target, const PoolsRequestedSequences& sequences)>;

class PoolSynchronizer {
public:
    explicit PoolSynchronizer(BlockChain* blockChain);

    void sync(RoundNumber roundNum, RoundNumber difference = kRoundDifferentForSync);
    void syncLastPool();
    void getBlockReply(PoolsBlock&& poolsBlock, const cs::PublicKey& sender);
    bool isSyncroStarted() const;

    void stop();

    cs::Sequence getMaxNeighbourSequence();
    // the last sequence this neighbour announced above ours, 0 if none
    cs::Sequence announcedSequence(const cs::PublicKey& key) const;
    static const RoundNumber kRoundDifferentForSync = values::kDefaultMetaStorageMaxSize;
    // peers asked first for the next blocks when they are neighbours, best first (empty: a random
    // neighbour, as usual). Set by the node after --resolve-fork: the peers that have the chosen block
    void setPreferredSources(std::vector<cs::PublicKey> sources) {
        preferredSources_.set(std::move(sources));
    }
    const std::vector<cs::PublicKey>& preferredSources() const {
        return preferredSources_.sources();
    }
    void getSyncroMessage(const cs::PublicKey& sender, SyncroMessage msg);

    static const size_t kFreeBlocksTimeoutMs = 10000;
    static const size_t kCachedBlocksLimit = 10000;

    //void trySource(cs::Sequence finSeq, cs::PublicKey& source);
    //void showNeighbours();
    //void syncTill(cs::Sequence finSeq, const cs::PublicKey& source, bool newCall);

public signals:
    PoolSynchronizerRequestSignal sendRequest;

public slots:
    void onStoreBlockTimeElapsed();
    void onPingReceived(Sequence sequence, const PublicKey& publicKey);
    void onNeighbourAdded(const PublicKey&, Sequence) {}
    void onNeighbourRemoved(const PublicKey& publicKey);

private slots:
    void onTimeOut();

private:
    void sendBlockRequest();
    void sendBlock(const PublicKey& neighbour, const PoolsRequestedSequences& sequences);

    void addSynchroLog(const cs::PublicKey& sender, cs::PoolsRequestedSequences& sequences, SyncroMessage msg);
    bool changeSynchroLog(const cs::PublicKey& sender, SyncroMessage msg);
    void updateSynchroLog();
    bool removeSynchroLog(const cs::PublicKey& sender);
    bool checkSynchroLog(const cs::PublicKey& sender);
    // the request log is kept for this peer: special order 35 applied here, or the peer sent a
    // SyncroMsg (its own anti-duplicate log is active)
    bool synchroLogFor(const cs::PublicKey& sender) const;

    bool showSyncronizationProgress(Sequence lastWrittenSequence) const;
    void manageSyncBlocks(cs::PoolsBlock&& poolsBlock);

    std::vector<Sequence> getNeededSequences(
        const std::vector<BlockChain::SequenceInterval>& requiredBlocks,
        Sequence neighbourLastSeq
    );

    void synchroFinished();
    size_t nextIndex(size_t index) const;
    // the neighbour to ask first: a preferred source (PreferredSources::pick), else a random one
    std::unordered_map<PublicKey, Sequence>::iterator firstNeighbour();

    BlockChain* blockChain_;

    std::atomic<bool> isSyncroStarted_ = false;
    std::atomic<bool> stopped_ = false;

    Sequence maxRequestedSequence_ = kWrongSequence;
    std::unordered_map<PublicKey, Sequence> neighbours_;
    PreferredSources preferredSources_;

    std::map<cs::PublicKey, std::tuple<cs::PoolsRequestedSequences, SyncroMessage, uint64_t>> synchroLog_;
    SyncroPacing pacing_;
    Timer timer_;
};
}  // namespace cs
#endif  // POOLSYNCHRONIZER_HPP
