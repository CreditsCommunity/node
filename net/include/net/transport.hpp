/* Send blaming letters to @yrtimd */
#ifndef TRANSPORT_HPP
#define TRANSPORT_HPP

#include <atomic>
#include <csignal>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>
#include <optional>

#include <config.hpp>

#include <lib/system/common.hpp>
#include <lib/system/logger.hpp>
#include <lib/system/signals.hpp>

#include <p2p_network.h>

#include "neighbourhood.hpp"
#include "packet.hpp"
#include "packetsqueue.hpp"

inline volatile std::sig_atomic_t gSignalStatus = 0;

using PingSignal = cs::Signal<void(cs::Sequence, const cs::PublicKey&)>;
using NeighbourAddedSignal = cs::Signal<void(const cs::PublicKey&, cs::Sequence, cs::RoundNumber)>;
using NeighbourRemovedSignal = cs::Signal<void(const cs::PublicKey&)>;

class Node;

class Transport : public net::HostEventHandler {
public:
    inline static volatile std::sig_atomic_t gSignalStatus = 0;   // legacy flag; new path is stop()
    void stop();   // wakes run loops immediately via cv

    using AddressAndPort = std::pair<std::string, uint16_t>;
    using BanList = std::vector<AddressAndPort>;
    enum ProcSpeed {
        setDefault,
        increase,
        decrease
    };
    explicit Transport(Node* node);
    ~Transport();

    void run();
    bool isGood() const { return good_; }

    void processNodeMessage(const cs::PublicKey&, const Packet&);
    void processPostponed(const cs::RoundNumber); // @TODO move to Node

    void sendDirect(Packet&&, const cs::PublicKey&);
    // only to a node of the routing table's k-buckets: no queue and no lookup for another id (to a
    // known node not yet connected the message waits in the send queue until the dial ends)
    bool sendDirectIfKnown(Packet&&, const cs::PublicKey&);
    void sendMulticast(Packet&&, const std::vector<cs::PublicKey>&);
    void sendBroadcast(Packet&&);
    void sendBroadcastIfNoConnection(Packet&&, const cs::PublicKey&);

    void ban(const cs::PublicKey&);
    void revertBan(const cs::PublicKey&);
    void clearBanList();
    void getBanList(BanList&) const;
    void clearInbox();
    // what a restart rebuilds in the network layer, without one (net::Host::Reset): connections closed,
    // the routing table looked up again, so the neighbours go and come back with a fresh handshake
    void resetNetwork();
    // a node behind a NAT keeps a connection of its own open to the peers of its routing table
    void keepReachable();

    // neighbours interface
    void setPermanentNeighbours(const std::set<cs::PublicKey>&);
    uint32_t getNeighboursCount() const;
    uint32_t getMaxNeighbours() const;
    void forEachNeighbour(Neighbourhood::NeighboursCallback);
    bool hasNeighbour(const cs::PublicKey&) const;
    void addToNeighbours(const std::set<cs::PublicKey>&);

    void getKnownPeers(std::vector<cs::PeerData>&);
    // the peers the routing table lost because they stopped answering and still pings again (p2p
    // LostPeers: each answered our pings at that endpoint before, at most 64, for 24 h at most)
    void getLostPeers(std::vector<cs::PeerData>&);
    // the capability byte a peer of the routing table announces (cs::hostdata); false if unknown
    bool peerCapabilities(const cs::PublicKey& key, uint8_t& capabilities);

    // from neigbours
    // @param added - true if new neighbour adder, false if removed
    void onNeighboursChanged(const cs::PublicKey&, cs::Sequence lastSeq,
                            cs::RoundNumber lastRound, bool added);
public slots:
    void onPingReceived(cs::Sequence sequence, const cs::PublicKey& key);

public signals:
    PingSignal pingReceived;
    cs::Action mainThreadIterated;
    NeighbourAddedSignal neighbourAdded;
    NeighbourRemovedSignal neighbourRemoved;

protected:
    // HostEventHandler
    void OnMessageReceived(const net::NodeId&, net::ByteVector&&) override;
    void OnNodeDiscovered(const net::NodeId&) override;
    void OnNodeRemoved(const net::NodeId&) override;

private:
// Postpone logic - beg
// @TODO move to Node
    void postponePacket(const cs::PublicKey& sender, const cs::RoundNumber, const Packet&);

    struct PostponedPack {
        cs::PublicKey sender;
        Packet pack;
    };
    std::map<cs::RoundNumber, std::vector<PostponedPack>> postponed_;
// Postpone logic - end

    void dispatchNodeMessage(const cs::PublicKey& sender, const MsgTypes,
                             const cs::RoundNumber, const uint8_t* data, size_t);
    void processorRoutine();
    void process();
    void checkNeighboursChange();

    bool good_ = false;
    net::Config config_;

    Node* node_;

    std::condition_variable newPacketsReceived_;
    std::mutex inboxMux_;
    PacketsQueue inboxQueue_;

    std::atomic<bool> stopped_{false};
    // where the next keepalive round starts among the routing table peers (keepReachable, run thread)
    size_t keepaliveOffset_ = 0;
    std::mutex shutdownMux_;
    std::condition_variable shutdownCv_;

    Neighbourhood neighbourhood_;
    std::thread processorThread_;

    struct NeighbourData {
        const cs::PublicKey key;
        cs::Sequence lastSeq;
        cs::RoundNumber lastRound;
        bool added; // true if should be added, false if should be removed

        NeighbourData(const cs::PublicKey& key, cs::Sequence s, cs::RoundNumber r, bool a)
            : key(key), lastSeq(s), lastRound(r), added(a) {}
    };

    std::mutex neighboursMux_;
    std::list<NeighbourData> neighboursToHandle_;

    net::Host host_;
};
#endif  // TRANSPORT_HPP
