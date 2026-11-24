#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include <p2p_network.h>

// net::Connection of the p2p fork (CreditsCommunity/p2p): what Host::Reset and Host::OnConnected rely
// on. A dial abandoned by Reset reports nothing and sends nothing; the dialled node's id stays the
// connection's own whatever id the other side registers with (OnConnected refuses a mismatch).
namespace {

using namespace net;

NodeId idOf(uint8_t b) {
    NodeId id;
    auto* p = reinterpret_cast<uint8_t*>(id.GetPtr());
    for (size_t i = 0; i < id.size(); ++i) {
        p[i] = b;
    }
    return id;
}

Packet registration(const NodeId& sender, const NodeId& receiver) {
    Packet p;
    p.data = {0, 127, 0, 0, 1, 0x10, 0x27};  // v4, 127.0.0.1, port 10000
    p.header.type = Packet::Type::kRegistration;
    p.header.data_size = p.data.size();
    p.header.sender = sender;
    p.header.receiver = receiver;
    return p;
}

ByteVector bytesOf(const Packet& p) {
    Serializer s;
    s.Put(p);
    return s.GetData();
}

class Owner : public ConnectionOwner {
public:
    void OnPacketReceived(Packet&&) override {
        ++packets;
    }
    void OnConnected(Packet&& pack, Connection::Ptr conn) override {
        std::lock_guard<std::mutex> g(mux);
        connectedSender = pack.header.sender;
        connectedRemote = conn->RemoteNode();
        ++connected;
    }
    void OnConnectionDropped(const NodeId&, bool, Connection::DropReason, const Connection*) override {
        ++dropped;
    }
    void OnPendingConnectionError(const NodeId&, Connection::DropReason) override {
        ++pendingErrors;
    }

    std::mutex mux;
    NodeId connectedSender;
    NodeId connectedRemote;
    std::atomic<int> connected{0};
    std::atomic<int> dropped{0};
    std::atomic<int> pendingErrors{0};
    std::atomic<int> packets{0};
};

struct Loop {
    ba::io_context io;
    ba::executor_work_guard<ba::io_context::executor_type> work = ba::make_work_guard(io);
    std::thread thread{[this] { io.run(); }};
    ~Loop() {
        work.reset();
        io.stop();
        thread.join();
    }
};

template <class F>
bool waitFor(F f, int ms = 3000) {
    for (int i = 0; i < ms / 10; ++i) {
        if (f()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return f();
}

}  // namespace

TEST(P2pConnection, DialKeepsTheDialledIdWhateverTheOtherSideRegisters) {
    Loop loop;
    bi::tcp::acceptor acceptor(loop.io, bi::tcp::endpoint(bi::make_address("127.0.0.1"), 0));
    Owner owner;
    const NodeId me = idOf(1), dialled = idOf(2), other = idOf(3);

    auto conn = Connection::Create(owner, loop.io);
    conn->Connect(acceptor.local_endpoint(), registration(me, dialled));
    bi::tcp::socket server(loop.io);
    acceptor.accept(server);
    // the dialled endpoint answers as another node
    ba::write(server, ba::buffer(bytesOf(registration(other, me))));

    ASSERT_TRUE(waitFor([&] { return owner.connected.load() == 1; }));
    std::lock_guard<std::mutex> g(owner.mux);
    EXPECT_TRUE(owner.connectedSender == other);
    EXPECT_TRUE(owner.connectedRemote == dialled);  // what Host::OnConnected compares and removes by
    conn->Abandon();
}

TEST(P2pConnection, AbandonedDialReportsNothingAndSendsNothing) {
    Loop loop;
    bi::tcp::acceptor acceptor(loop.io, bi::tcp::endpoint(bi::make_address("127.0.0.1"), 0));
    Owner owner;
    const NodeId me = idOf(1), dialled = idOf(2);

    auto conn = Connection::Create(owner, loop.io);
    conn->Connect(acceptor.local_endpoint(), registration(me, dialled));
    bi::tcp::socket server(loop.io);
    acceptor.accept(server);

    conn->Abandon();
    EXPECT_TRUE(conn->IsDropped());
    // refused: a dropped connection queues nothing
    Packet p;
    p.data = ByteVector(64, 7);
    p.header.type = Packet::Type::kDirect;
    p.header.data_size = p.data.size();
    conn->Send(std::move(p));

    // the other side gets at most the registration written before, then the end of the stream
    std::vector<uint8_t> got;
    boost::system::error_code ec;
    std::array<uint8_t, 4096> buf{};
    for (;;) {
        const auto n = server.read_some(ba::buffer(buf), ec);
        if (ec) {
            break;
        }
        got.insert(got.end(), buf.begin(), buf.begin() + n);
    }
    EXPECT_LE(got.size(), bytesOf(registration(me, dialled)).size());

    // and the owner hears nothing: no drop, no pending error (no unreachable mark), no connection
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(owner.connected.load(), 0);
    EXPECT_EQ(owner.dropped.load(), 0);
    EXPECT_EQ(owner.pendingErrors.load(), 0);
}

TEST(P2pConnection, AbandonedAfterRegistrationIsReportedAsDropped) {
    // Host::OnConnected checks IsDropped under its lock: a connection abandoned while its registration
    // was being read is never taken
    Loop loop;
    bi::tcp::acceptor acceptor(loop.io, bi::tcp::endpoint(bi::make_address("127.0.0.1"), 0));
    Owner owner;
    const NodeId me = idOf(1), dialled = idOf(2);

    auto conn = Connection::Create(owner, loop.io);
    conn->Connect(acceptor.local_endpoint(), registration(me, dialled));
    bi::tcp::socket server(loop.io);
    acceptor.accept(server);
    ba::write(server, ba::buffer(bytesOf(registration(dialled, me))));
    ASSERT_TRUE(waitFor([&] { return owner.connected.load() == 1; }));
    EXPECT_FALSE(conn->IsDropped());
    conn->Abandon();
    EXPECT_TRUE(conn->IsDropped());
}
