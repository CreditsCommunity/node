#define TESTING

#include <solver/smartcontracts.hpp>
#include <solver/statefetcher.hpp>
#include "clientconfigmock.hpp"
#include "gtest/gtest.h"
#include <csdb/transaction.hpp>
#include <apihandler.hpp>
#include <base58.h>
#include <serializer.hpp>
#include <config.hpp>
#include <csnode/blockchain.hpp>
#include <csnode/configholder.hpp>
#include <solver/callsqueuescheduler.hpp>
#include "memorydb.hpp"
//#include "networkmock.hpp"
//#include "nodemock.hpp"
//#include "solvermock.hpp"
//#include "transportmock.hpp"

TEST(SmartContract, basic_checks) {
    using namespace cs;
    cs::Signature dummy_sign{};

    cscrypto::Bytes bytes;
    ASSERT_TRUE(DecodeBase58("Axu1s33E9taieswhainpzjt4M5WieYNKHRfTtbpajPoc", bytes));
    csdb::Address src = csdb::Address::from_public_key(bytes);
    ASSERT_TRUE(DecodeBase58("4xho6RB8erqziK4jiyYdGX7YUXp7J2i3YoGSu96SEUJf", bytes));
    csdb::Address tgt = csdb::Address::from_public_key(bytes);

    // basic transaction
    csdb::Transaction t(1LL, src, tgt, csdb::Currency{}, csdb::Amount(0), csdb::AmountCommission(0.0), csdb::AmountCommission(0.0), dummy_sign);
    ASSERT_TRUE(t.is_valid());

    // deploy
    api::SmartContractDeploy api_deploy_data;
    api_deploy_data.__set_sourceCode("contract code");
    api::SmartContractInvocation api_deploy_invoke;
    api_deploy_invoke.__set_smartContractDeploy(api_deploy_data);
    csdb::Transaction t_deploy = t.clone();
    t_deploy.add_user_field(trx_uf::deploy::Code, cs::Serializer::serialize(api_deploy_invoke));

    // call (start)
    api::SmartContractInvocation api_call_invoke;
    api_call_invoke.__set_method("method");
    csdb::Transaction t_start = t.clone();
    t_start.add_user_field(trx_uf::start::Methods, cs::Serializer::serialize(api_call_invoke));

    // new state
    csdb::Transaction t_state = t.clone();
    t_state.add_user_field(trx_uf::new_state::Hash, std::string{});
    ASSERT_FALSE(SmartContracts::is_new_state(t_state)); // not enough feilds
    t_state.add_user_field(trx_uf::new_state::RefStart, SmartContractRef().to_user_field());
    ASSERT_TRUE(SmartContracts::is_new_state(t_state)); // ok
    t_state.add_user_field(trx_uf::new_state::RetVal, "text"); // optional
    t_state.add_user_field(trx_uf::new_state::Fee, csdb::Amount(0)); // optional

    ASSERT_TRUE(SmartContracts::is_executable(t_deploy));
    ASSERT_TRUE(SmartContracts::is_executable(t_start));
    ASSERT_FALSE(SmartContracts::is_executable(t_state));

    ASSERT_TRUE(SmartContracts::is_deploy(t_deploy));
    ASSERT_FALSE(SmartContracts::is_deploy(t_start));
    ASSERT_FALSE(SmartContracts::is_deploy(t_state));

    ASSERT_FALSE(SmartContracts::is_start(t_deploy));
    ASSERT_TRUE(SmartContracts::is_start(t_start));
    ASSERT_FALSE(SmartContracts::is_start(t_state));

    ASSERT_FALSE(SmartContracts::is_new_state(t_deploy));
    ASSERT_FALSE(SmartContracts::is_new_state(t_start));
    ASSERT_TRUE(SmartContracts::is_new_state(t_state));

}

// a contract left locked by an aborted call is unlocked, while the contracts that
// a running call executes or uses stay locked
TEST(SmartContract, stale_locks_keeps_used_contracts) {
    using namespace cs;
    auto address = [](uint8_t n) {
        cs::PublicKey key{};
        key[0] = n;
        return csdb::Address::from_public_key(key);
    };
    const auto running = address(1);
    const auto used = address(2);
    const auto stale = address(3);

    const std::unordered_set<csdb::Address> locked{running, used, stale};
    const auto result = SmartContracts::stale_locks(locked, {running, used});
    ASSERT_EQ(result.size(), 1u);
    ASSERT_EQ(result.front(), stale);

    ASSERT_TRUE(SmartContracts::stale_locks(locked, {running, used, stale}).empty());
    ASSERT_EQ(SmartContracts::stale_locks(locked, {address(9)}).size(), 3u);
}

namespace {
csdb::Address addressOf(uint8_t b) {
    return csdb::Address::from_public_key(cs::Bytes(32, b));
}

csdb::Transaction plain(csdb::Address src, csdb::Address tgt, int64_t id) {
    return csdb::Transaction(id, src, tgt, csdb::Currency{}, csdb::Amount(1), csdb::AmountCommission(0.0), csdb::AmountCommission(0.0), cs::Signature{});
}

cs::SmartContractRef refOf(cs::Sequence seq, cs::Sequence index) {
    cs::SmartContractRef ref;
    ref.sequence = seq;
    ref.transaction = index;
    return ref;
}

// a new_state of contract `contract` for the execution started at seq.index, stored as a hash (a zero
// hash declares a failed execution)
csdb::Transaction newState(csdb::Address contract, cs::Sequence seq, cs::Sequence index, int64_t id, char hashByte = 'h') {
    auto t = plain(contract, contract, id);
    t.add_user_field(cs::trx_uf::new_state::Hash, std::string(32, hashByte));
    t.add_user_field(cs::trx_uf::new_state::RefStart, refOf(seq, index).to_user_field());
    return t;
}

// the same, with the state value in the block (older blocks): an empty value declares a failed execution
csdb::Transaction newStateWithValue(csdb::Address contract, cs::Sequence seq, cs::Sequence index, int64_t id, const std::string& value) {
    auto t = plain(contract, contract, id);
    t.add_user_field(cs::trx_uf::new_state::Value, value);
    t.add_user_field(cs::trx_uf::new_state::RefStart, refOf(seq, index).to_user_field());
    return t;
}

csdb::Address addressOf(const char* base58) {
    cs::Bytes bytes;
    EXPECT_TRUE(DecodeBase58(base58, bytes));
    return csdb::Address::from_public_key(bytes);
}

const auto sameAddress = [](const csdb::Address& a) { return a; };

// block 25,897,074 of the mainnet, as stored: two replenishes of EYvmJjyc (2bAyU7bF 0.7 at 25,897,070.0,
// GZNHaxfo 0.1 at 25,897,070.1) executed in one batch, each new_state followed by its payout
struct Block25897074 {
    const csdb::Address dice = addressOf("EYvmJjycxhgxXjueZPzUAGvcZaTctWE8dMKhgY7A1gUH");
    const csdb::Address other = addressOf("TmrvEvosUF4TC78NMcEz7n27WGqvJapPabY5TGQ9zPg");
    const csdb::Address first = addressOf("2bAyU7bFW6C2XevacDoKaoHVUucwMLk6DkM94kXnUMfa");
    const csdb::Address second = addressOf("GZNHaxfoZUbNhcf8Hsa6aAaNJcAg8y7Tc1EgNHeEWCi5");
    const csdb::Address player = addressOf("89VMiM8xtPaVkU2arxdtB5yiSfdZnX6BSiHjCNECdh9G");
    csdb::Pool block{csdb::PoolHash{}, 25897074};

    Block25897074() {
        block.add_transaction(plain(first, dice, 1));             // 0: 2bAyU7bF replenishes EYvmJjyc
        block.add_transaction(plain(player, other, 2));           // 1: 89VMiM8x replenishes TmrvEvos
        block.add_transaction(newState(dice, 25897070, 0, 3));    // 2: new_state of 25,897,070.0 (2bAyU7bF)
        block.add_transaction(plain(dice, first, 4));             // 3: its payout, 1.4 to 2bAyU7bF
        block.add_transaction(newState(dice, 25897070, 1, 5));    // 4: new_state of 25,897,070.1 (GZNHaxfo)
        block.add_transaction(plain(dice, second, 6));            // 5: its payout, 0.2 to GZNHaxfo
    }
};
}  // namespace

// The fee of an emitted transaction goes to the initiator of the contract's nearest new_state before it
// in the block (the sequential rule), as the mainnet majority holds for block 25,897,074: the payout
// 25,897,074.3 is charged to 2bAyU7bF, 25,897,074.5 to GZNHaxfo. The latest-execution rule of #94
// charged both to GZNHaxfo, the rebuild's 109-wallet difference.
TEST(SmartContract, EmittedTransactionIsChargedToThePrecedingExecutionInTheBlock) {
    const Block25897074 b;
    const auto pay1 = cs::SmartContracts::emitting_new_state(b.block, 3, b.dice, sameAddress);
    ASSERT_TRUE(pay1.has_value());
    EXPECT_EQ(*pay1, refOf(25897070, 0));
    const auto pay2 = cs::SmartContracts::emitting_new_state(b.block, 5, b.dice, sameAddress);
    ASSERT_TRUE(pay2.has_value());
    EXPECT_EQ(*pay2, refOf(25897070, 1));
    // another contract's new_states do not count, and nothing precedes the first new_state
    EXPECT_FALSE(cs::SmartContracts::emitting_new_state(b.block, 3, b.other, sameAddress).has_value());
    EXPECT_FALSE(cs::SmartContracts::emitting_new_state(b.block, 2, b.dice, sameAddress).has_value());
}

// 2019: block 10,635,589 holds twelve executions of 8tvGa1U6 (the replenishes 10,635,585.0-.11 of the
// "TiMo" players), each new_state followed by the payout to its player. Each payout is charged to its own
// player; the latest rule charged all twelve to FVhwTiMo (10,635,585.11).
TEST(SmartContract, EmittedTransactionsOfTwelveExecutionsIn2019) {
    const auto contract = addressOf("8tvGa1U6aHyH35SEzoNFLQd7jWNis3vaDMyLYjXPEN2x");
    const char* players[] = {"ETiMoag8S8GkrAoe2NGkDm9j1QQ5m3RKaoNz6Vqd6qfY", "HtimoDtTYGSVotnQ5Eo4eud3FkDv5r2QYiKSZcdWP7Z8",
                             "EtimoSnoirCSsCfUFx6JiFJ7rT8nBr9DmHactnJBnCUp", "8SzsTiMosxsm2B3j2gYBEAxe1uXmD6yC5WXWPHcFi4Ui",
                             "HTQTiMoV63hrdktseere51jBhqApmqsKq9h9agDK8h8k", "HquTiMok5gPj4Z9PCCugPtZKdeWv9utPbAkAYt1bgP2Y",
                             "8qTimorPDG4RwFZdRDhpiZy7A4cnqeWjcpiw6ktu3DUR", "6ptiMomruXxYPdQ9mDLZKw2a8U7zwmTR75QpGzyzd6JZ",
                             "AsiTiMok3AVw26wyDTkS8Fk2idtZGBSWTxryfR7pQ1Pk", "9tiMoYhVF6vTMP37ddbubMKSLWctQeZEAzneqLgFxzRr",
                             "3atimow5wVqdNH6612C1AZHXWALW35w8HzHDhYfrfXAf", "FVhwTiMoT2g3wXh9DnE7nkUBcRoudF4DM9ex1QWhsGt4"};
    csdb::Pool block(csdb::PoolHash{}, 10635589);
    for (uint8_t k = 0; k < 12; ++k) {
        block.add_transaction(newState(contract, 10635585, k, 2 * k + 1));          // 2k: new_state of 10,635,585.k
        block.add_transaction(plain(contract, addressOf(players[k]), 2 * k + 2));   // 2k+1: the payout to its player
    }
    for (size_t k = 0; k < 12; ++k) {
        const auto ref = cs::SmartContracts::emitting_new_state(block, 2 * k + 1, contract, sameAddress);
        ASSERT_TRUE(ref.has_value()) << players[k];
        EXPECT_EQ(*ref, refOf(10635585, k)) << players[k];
    }
}

// Only what the block declares counts: a failed execution (zero hash, or an empty value in an older
// block) is passed over, a successful one counts whether or not this node reproduced its state (the
// rule never reads the node's state, so a re-execution that failed while syncing changes nothing).
TEST(SmartContract, EmittedTransactionCountsOnlyDeclaredSuccessfulExecutions) {
    const auto A = addressOf(1), B = addressOf(2), player = addressOf(3);
    csdb::Pool block(csdb::PoolHash{}, 50);
    block.add_transaction(newState(A, 40, 0, 1));                      // 0: A's execution 40.0
    block.add_transaction(plain(A, player, 2));                        // 1: its payout
    block.add_transaction(newState(B, 41, 1, 3));                      // 2: B's execution 41.1
    block.add_transaction(newState(A, 45, 2, 4, '\0'));               // 3: A's 45.2 failed (zero hash)
    block.add_transaction(plain(A, player, 5));                        // 4: still 40.0's
    block.add_transaction(plain(B, player, 6));                        // 5: B's, past A's new_states
    block.add_transaction(newStateWithValue(A, 46, 0, 7, "state"));    // 6: A's 46.0, value in the block
    block.add_transaction(newStateWithValue(A, 47, 0, 8, ""));         // 7: A's 47.0 failed (empty value)
    block.add_transaction(plain(A, player, 9));                        // 8: 46.0's

    EXPECT_EQ(*cs::SmartContracts::emitting_new_state(block, 1, A, sameAddress), refOf(40, 0));
    EXPECT_EQ(*cs::SmartContracts::emitting_new_state(block, 4, A, sameAddress), refOf(40, 0));
    EXPECT_EQ(*cs::SmartContracts::emitting_new_state(block, 5, B, sameAddress), refOf(41, 1));
    EXPECT_EQ(*cs::SmartContracts::emitting_new_state(block, 8, A, sameAddress), refOf(46, 0));

    // no successful new_state before it: the caller falls back to the execution before the block
    csdb::Pool other(csdb::PoolHash{}, 51);
    other.add_transaction(newState(A, 48, 0, 10, '\0'));
    other.add_transaction(plain(A, player, 11));
    EXPECT_FALSE(cs::SmartContracts::emitting_new_state(other, 1, A, sameAddress).has_value());
    EXPECT_EQ(cs::SmartContracts::charged_execution(std::nullopt, refOf(46, 0)), refOf(46, 0));
}

// H1: among the successful new_states before the payout, the highest RefStart, not the nearest: a node
// applying the block in order ignores a state older than the contract's execution (update_contract_state,
// "ignore outdated update", also in official 4a95aaaf), so S(.1), S(.0), E leaves the contract at .1. In
// an ordered block (every real one) the highest is the nearest. Never below the execution before the block.
TEST(SmartContract, EmittedTransactionIsChargedToTheHighestPrecedingExecution) {
    const auto A = addressOf(1), player = addressOf(3);
    csdb::Pool block(csdb::PoolHash{}, 50);
    block.add_transaction(newState(A, 45, 1, 1));    // 0: S(45.1)
    block.add_transaction(newState(A, 45, 0, 2));    // 1: S(45.0), outdated after 45.1
    block.add_transaction(plain(A, player, 3));      // 2: E
    const auto emitting = cs::SmartContracts::emitting_new_state(block, 2, A, sameAddress);
    ASSERT_TRUE(emitting.has_value());
    EXPECT_EQ(*emitting, refOf(45, 1));
    EXPECT_EQ(cs::SmartContracts::charged_execution(emitting, refOf(40, 0)), refOf(45, 1));

    // the execution before the block is the floor: an outdated state does not move the charge below it
    EXPECT_EQ(cs::SmartContracts::charged_execution(emitting, refOf(46, 0)), refOf(46, 0));
    EXPECT_EQ(cs::SmartContracts::charged_execution(emitting, refOf(45, 1)), refOf(45, 1));
    // an unknown execution before the block (invalid ref) is no floor
    EXPECT_EQ(cs::SmartContracts::charged_execution(emitting, cs::SmartContractRef{}), refOf(45, 1));
}

namespace {
// a call of `caller` to `contract` (a start transaction)
csdb::Transaction callOf(csdb::Address caller, csdb::Address contract, int64_t id) {
    auto t = plain(caller, contract, id);
    api::SmartContractInvocation invocation;
    invocation.__set_method("play");
    t.add_user_field(cs::trx_uf::start::Methods, cs::Serializer::serialize(invocation));
    return t;
}

// a payout of `contract` to `to`, with a counted fee
csdb::Transaction payout(csdb::Address contract, csdb::Address to, int64_t id) {
    return csdb::Transaction(id, contract, to, csdb::Currency{}, csdb::Amount(1), csdb::AmountCommission(0.1),
                             csdb::AmountCommission(0.008740234375), cs::Signature{});
}

// SmartContracts on a BlockChain whose storage is an in-memory database. The last block holds the
// contract's calls (block 1000: .0 by `before` the contract's execution before the payout block, .1 by
// `first`, .2 by `second`), so the starters load from it. The payout block is 1001, the one a node builds
// next (on_update keys the executions it saves with bc.getLastSeq() + 1). on_next_block_impl, on_update and
// on_remove_block run as on a node; the charges and refunds are read from the signals the wallet cache
// takes, as the initiators they name.
class EmittedFee : public ::testing::Test {
protected:
    const csdb::Address dice = addressOf("EYvmJjycxhgxXjueZPzUAGvcZaTctWE8dMKhgY7A1gUH");
    const csdb::Address prior = addressOf("89VMiM8xtPaVkU2arxdtB5yiSfdZnX6BSiHjCNECdh9G");
    const csdb::Address first = addressOf("2bAyU7bFW6C2XevacDoKaoHVUucwMLk6DkM94kXnUMfa");
    const csdb::Address second = addressOf("GZNHaxfoZUbNhcf8Hsa6aAaNJcAg8y7Tc1EgNHeEWCi5");
    const csdb::Address player = addressOf(7);
    static constexpr cs::Sequence kCalls = 1000;
    static constexpr cs::Sequence kPayouts = 1001;

    BlockChain chain{csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000001"),
                     csdb::Address::from_string("0000000000000000000000000000000000000000000000000000000000000002")};
    CallsQueueScheduler scheduler;
    std::shared_ptr<MemoryDb> db = std::make_shared<MemoryDb>();
    csdb::Pool calls{csdb::PoolHash{}, kCalls};

    struct Peer {
        std::unique_ptr<cs::SmartContracts> sc;
        std::vector<csdb::Address> charged;    // the initiator charged, payout by payout
        std::vector<csdb::Address> refunded;   // the initiator refunded, in on_remove_block's order
    };

    void SetUp() override {
        cs::ConfigHolder::instance().setConfig(Config{});
        csdb::Storage::OpenOptions options;
        options.db = db;
        ASSERT_TRUE(chain.test_storage().open(options));
        calls.add_transaction(callOf(prior, dice, 1));
        calls.add_transaction(callOf(first, dice, 2));
        calls.add_transaction(callOf(second, dice, 3));
        ASSERT_TRUE(calls.compose());
        chain.test_setLastBlock(calls);
    }

    std::unique_ptr<Peer> node() {
        auto n = std::make_unique<Peer>();
        n->sc = std::make_unique<cs::SmartContracts>(chain, scheduler);
        // the contract stands at its execution before the payout block, kCalls.0
        n->sc->test_addContract(dice, refOf(kCalls, 0), calls.transaction(0), "state 0");
        Peer* p = n.get();
        cs::Connector::connect(&n->sc->signal_emitted_accepted,
                               [p](const csdb::Transaction&, const csdb::Transaction& starter) { p->charged.push_back(starter.source()); });
        cs::Connector::connect(&n->sc->rollback_emitted_accepted,
                               [p](const csdb::Transaction&, const csdb::Transaction& starter) { p->refunded.push_back(starter.source()); });
        return n;
    }

    // the new_states of a block, as the conveyer passes them to statesCreated
    static std::vector<csdb::Transaction> statesOf(const csdb::Pool& block) {
        std::vector<csdb::Transaction> states;
        for (const auto& t : block.transactions()) {
            if (cs::SmartContracts::is_new_state(t)) {
                states.push_back(t);
            }
        }
        return states;
    }

    // a node that follows the rounds live: statesCreated -> on_update when the round's block is formed,
    // then the block is stored
    void live(Peer& n, const csdb::Pool& block) {
        n.sc->on_update(statesOf(block));
        n.sc->test_nextBlock(block, false);
    }

    // a node that reads the block (sync, start-up)
    void reading(Peer& n, const csdb::Pool& block) {
        n.sc->test_nextBlock(block, true);
    }

    csdb::Transaction state(cs::Sequence index, int64_t id) {
        return newStateWithValue(dice, kCalls, index, id, "state " + std::to_string(index));
    }
};
}  // namespace

// The layout of block 25,897,074 (a new_state, its payout, the next new_state, its payout) with a payout
// first that no new_state precedes. A node that follows the rounds live has applied both states before it stores
// it (on_update: ref_execute is at .2), a node that reads it applies them in order: both charge the
// sequential result, the first payout to the execution before the block, then 2bAyU7bF, then GZNHaxfo.
TEST_F(EmittedFee, LiveAndReadingNodesChargeTheSameInitiators) {
    csdb::Pool block(csdb::PoolHash{}, kPayouts);
    block.add_transaction(payout(dice, player, 10));   // 0: no new_state before it
    block.add_transaction(state(1, 11));               // 1: S(.1) by 2bAyU7bF
    block.add_transaction(payout(dice, first, 12));    // 2
    block.add_transaction(state(2, 13));               // 3: S(.2) by GZNHaxfo
    block.add_transaction(payout(dice, second, 14));   // 4
    ASSERT_TRUE(block.compose());   // as stored: the transactions get their ids

    auto building = node();
    building->sc->on_update(statesOf(block));
    EXPECT_EQ(building->sc->test_refExecute(dice), refOf(kCalls, 2));   // the states are applied ahead
    building->sc->test_nextBlock(block, false);
    auto reader = node();
    reading(*reader, block);

    const std::vector<csdb::Address> sequential{prior, first, second};
    EXPECT_EQ(building->charged, sequential);
    EXPECT_EQ(reader->charged, sequential);
    EXPECT_EQ(reader->sc->test_refExecute(dice), refOf(kCalls, 2));
    // the saved executions are dropped after the block; the first payout's charge is recorded
    EXPECT_EQ(building->sc->test_executionsBeforeBlock(), 0u);
    EXPECT_EQ(building->sc->test_fallbackCharges(), 1u);
    EXPECT_EQ(reader->sc->test_fallbackCharges(), 1u);
}

// H1 on the node: S(.2), S(.1), E. The reading node ignores the outdated .1 and holds .2; the live node
// applied both; both charge GZNHaxfo (.2), not 2bAyU7bF (the nearest).
TEST_F(EmittedFee, AnOutdatedStateDoesNotMoveTheCharge) {
    csdb::Pool block(csdb::PoolHash{}, kPayouts);
    block.add_transaction(state(2, 11));
    block.add_transaction(state(1, 12));
    block.add_transaction(payout(dice, second, 13));
    ASSERT_TRUE(block.compose());
    auto building = node();
    live(*building, block);
    auto reader = node();
    reading(*reader, block);
    EXPECT_EQ(building->charged, std::vector<csdb::Address>{second});
    EXPECT_EQ(reader->charged, std::vector<csdb::Address>{second});
    EXPECT_EQ(reader->sc->test_refExecute(dice), refOf(kCalls, 2));
}

// C3: on_update keeps the executions for the block it builds (bc.getLastSeq() + 1); a block of another
// sequence does not use them, and on_remove_block drops them.
TEST_F(EmittedFee, SavedExecutionsServeOnlyTheBlockTheyWereSavedFor) {
    csdb::Pool built(csdb::PoolHash{}, kPayouts);
    built.add_transaction(payout(dice, player, 10));
    built.add_transaction(state(1, 11));
    built.add_transaction(payout(dice, first, 12));
    ASSERT_TRUE(built.compose());

    // the saved execution .0 is used for block 1001: the first payout goes to the prior caller
    auto n = node();
    live(*n, built);
    EXPECT_EQ(n->charged, (std::vector<csdb::Address>{prior, first}));

    // saved for block 1001, then a block 1002 arrives: the saved .0 is not used, the contract stands at .1
    auto other = node();
    other->sc->on_update(statesOf(built));
    EXPECT_EQ(other->sc->test_executionsBeforeBlock(), 1u);
    csdb::Pool next(csdb::PoolHash{}, kPayouts + 1);
    next.add_transaction(payout(dice, player, 20));
    ASSERT_TRUE(next.compose());
    other->sc->test_nextBlock(next, false);
    EXPECT_EQ(other->charged, std::vector<csdb::Address>{first});
    EXPECT_EQ(other->sc->test_executionsBeforeBlock(), 0u);

    // a block removed after on_update drops the saved executions
    auto removing = node();
    removing->sc->on_update(statesOf(built));
    EXPECT_EQ(removing->sc->test_executionsBeforeBlock(), 1u);
    csdb::Pool removed(csdb::PoolHash{}, kCalls);
    removed.add_transaction(payout(dice, player, 30));
    ASSERT_TRUE(removed.compose());
    removing->sc->on_remove_block(removed);
    EXPECT_EQ(removing->sc->test_executionsBeforeBlock(), 0u);

    // clear() drops them and the recorded charges (C2)
    auto cleared = node();
    live(*cleared, built);
    EXPECT_EQ(cleared->sc->test_fallbackCharges(), 1u);
    cleared->sc->on_update(statesOf(built));
    cleared->sc->clear();
    EXPECT_EQ(cleared->sc->test_fallbackCharges(), 0u);
    EXPECT_EQ(cleared->sc->test_executionsBeforeBlock(), 0u);
}

// A rollback refunds each payout to the initiator it was charged to: on_remove_block walks the block
// backwards; the first payout's charge (no new_state before it) comes from the record on_next_block_impl
// made, since ref_execute has moved on to .2 by then.
TEST_F(EmittedFee, RollbackRefundsTheChargedInitiators) {
    csdb::Pool block(csdb::PoolHash{}, kPayouts);
    block.add_transaction(payout(dice, player, 10));
    block.add_transaction(state(1, 11));
    block.add_transaction(payout(dice, first, 12));
    block.add_transaction(state(2, 13));
    block.add_transaction(payout(dice, second, 14));
    ASSERT_TRUE(block.compose());
    for (bool is_live : {true, false}) {
        auto n = node();
        if (is_live) {
            live(*n, block);
        }
        else {
            reading(*n, block);
        }
        ASSERT_EQ(n->charged, (std::vector<csdb::Address>{prior, first, second})) << is_live;
        n->sc->on_remove_block(block);
        EXPECT_EQ(n->refunded, (std::vector<csdb::Address>{second, first, prior})) << is_live;
        EXPECT_EQ(n->sc->test_fallbackCharges(), 0u) << is_live;
    }
}

// The runtime warning: a payout that official nodes following the rounds live charge to another wallet
// (the contract's latest execution in the block) is detected, one entry per payout, the fee included;
// nothing is reported when the latest execution has the same initiator or the block has one execution.
TEST_F(EmittedFee, PayoutsChargedDifferentlyByLiveOfficialNodesAreReported) {
    csdb::Pool block(csdb::PoolHash{}, kPayouts);
    block.add_transaction(state(1, 11));               // 0: S(.1) by 2bAyU7bF
    block.add_transaction(payout(dice, first, 12));    // 1: live official nodes charge GZNHaxfo
    block.add_transaction(state(2, 13));               // 2: S(.2) by GZNHaxfo
    block.add_transaction(payout(dice, second, 14));   // 3: the same on both
    ASSERT_TRUE(block.compose());
    for (bool is_live : {true, false}) {
        auto n = node();
        if (is_live) {
            live(*n, block);
        }
        else {
            reading(*n, block);
        }
        EXPECT_EQ(n->charged, (std::vector<csdb::Address>{first, second})) << is_live;
        const auto& found = n->sc->test_emittedFeeDivergences();
        ASSERT_EQ(found.size(), 1u) << is_live;
        EXPECT_EQ(found[0].tr_idx, 1u);
        EXPECT_EQ(found[0].contract, dice);
        EXPECT_EQ(found[0].charged, refOf(kCalls, 1));
        EXPECT_EQ(found[0].live, refOf(kCalls, 2));
        EXPECT_EQ(found[0].payer, first);
        EXPECT_EQ(found[0].live_payer, second);
        EXPECT_GT(found[0].fee.to_double(), 0.008);

        const std::string line = cs::SmartContracts::emitted_fee_warning(kPayouts, found);
        EXPECT_NE(line.find("EMITTED FEE RULE: block 1'001"), std::string::npos) << line;
        EXPECT_NE(line.find("EYvmJjycxhgxXjueZPzUAGvcZaTctWE8dMKhgY7A1gUH"), std::string::npos) << line;
        EXPECT_NE(line.find("charged to 2bAyU7bFW6C2XevacDoKaoHVUucwMLk6DkM94kXnUMfa"), std::string::npos) << line;
        EXPECT_NE(line.find("live official nodes charge GZNHaxfoZUbNhcf8Hsa6aAaNJcAg8y7Tc1EgNHeEWCi5"), std::string::npos) << line;
        EXPECT_NE(line.find("2bAyU7bFW6C2XevacDoKaoHVUucwMLk6DkM94kXnUMfa -0.00"), std::string::npos) << line;
        EXPECT_NE(line.find("GZNHaxfoZUbNhcf8Hsa6aAaNJcAg8y7Tc1EgNHeEWCi5 +0.00"), std::string::npos) << line;
    }
    EXPECT_TRUE(cs::SmartContracts::emitted_fee_warning(kPayouts, {}).empty());

    // one execution in the block: nothing to report
    csdb::Pool single(csdb::PoolHash{}, kPayouts);
    single.add_transaction(state(1, 11));
    single.add_transaction(payout(dice, first, 12));
    ASSERT_TRUE(single.compose());
    auto n = node();
    reading(*n, single);
    EXPECT_TRUE(n->sc->test_emittedFeeDivergences().empty());
}

// The same caller started both executions: the rules name different executions but the same wallet pays,
// so no balance differs and nothing is reported.
TEST_F(EmittedFee, TheSameInitiatorIsNotReported) {
    calls = csdb::Pool(csdb::PoolHash{}, kCalls);
    calls.add_transaction(callOf(prior, dice, 1));
    calls.add_transaction(callOf(first, dice, 2));
    calls.add_transaction(callOf(first, dice, 3));
    ASSERT_TRUE(calls.compose());
    chain.test_setLastBlock(calls);
    csdb::Pool block(csdb::PoolHash{}, kPayouts);
    block.add_transaction(state(1, 11));
    block.add_transaction(payout(dice, first, 12));
    block.add_transaction(state(2, 13));
    block.add_transaction(payout(dice, first, 14));
    ASSERT_TRUE(block.compose());
    auto n = node();
    reading(*n, block);
    EXPECT_EQ(n->charged, (std::vector<csdb::Address>{first, first}));
    EXPECT_TRUE(n->sc->test_emittedFeeDivergences().empty());
}

// step 2 of the contract state sync plan: the declared call and hash survive a checkpoint, an item of
// an earlier build loads without them, and staleness compares the state with the declared hash
TEST(SmartContract, DeclaredStateSurvivesCheckpointAndOldItemsLoad) {
    cs::SmartContracts::StateItem item;
    item.ref_execute.sequence = 7;
    item.ref_execute.transaction = 1;
    item.state = "state after 7.1";
    item.declared_ref = item.ref_execute;
    item.declared_hash = cscrypto::calculateHash(reinterpret_cast<const cs::Byte*>(item.state.data()), item.state.size());

    auto bytes = item.to_bytes();
    const auto back = cs::SmartContracts::StateItem::from_bytes(bytes);
    EXPECT_EQ(back.declared_ref.sequence, 7u);
    EXPECT_EQ(back.declared_ref.transaction, 1u);
    EXPECT_EQ(back.declared_hash, item.declared_hash);
    EXPECT_EQ(back.state, item.state);

    // an item without a declaration keeps the bytes of earlier builds (their checkpoints are
    // verified against hashes of these bytes)
    cs::SmartContracts::StateItem plainItem = item;
    plainItem.declared_ref = cs::SmartContractRef{};
    plainItem.declared_hash = cs::Hash{};
    EXPECT_EQ(plainItem.to_bytes().size(), item.to_bytes().size() - (sizeof(item.declared_ref.sequence) + sizeof(item.declared_ref.transaction) + item.declared_hash.size()));

    // an earlier build wrote no declaration: the same bytes without the tail
    auto old = item.to_bytes();
    old.resize(old.size() - (sizeof(item.declared_ref.sequence) + sizeof(item.declared_ref.transaction) + item.declared_hash.size()));
    const auto earlier = cs::SmartContracts::StateItem::from_bytes(old);
    EXPECT_EQ(earlier.state, item.state);
    EXPECT_FALSE(earlier.declared_ref.is_valid());
    EXPECT_EQ(earlier.declared_hash, cs::Hash{});
}

TEST(SmartContract, StaleMeansNotTheDeclaredState) {
    cs::SmartContracts::StateItem item;
    item.state = "current";
    EXPECT_FALSE(cs::SmartContracts::is_stale(item));            // nothing declared: unknown, not stale
    item.declared_hash = cscrypto::calculateHash(reinterpret_cast<const cs::Byte*>(item.state.data()), item.state.size());
    EXPECT_FALSE(cs::SmartContracts::is_stale(item));            // holds the declared state
    item.state = "an earlier one";
    EXPECT_TRUE(cs::SmartContracts::is_stale(item));             // another state
    item.state.clear();
    EXPECT_TRUE(cs::SmartContracts::is_stale(item));             // no state at all
}

// step 3 of the contract state sync plan: a peer's state is taken only for the declared call and hash
TEST(SmartContract, StateReplyIsCheckedAgainstTheDeclaration) {
    using Check = cs::SmartContracts::StateReplyCheck;
    cs::SmartContracts::StateItem item;
    const std::string good = "state after 9.2";
    cs::SmartContractRef declared;
    declared.sequence = 9;
    declared.transaction = 2;
    cs::SmartContractRef older = declared, newer = declared;
    older.sequence = 8;
    newer.sequence = 10;

    EXPECT_EQ(cs::SmartContracts::verify_state_reply(declared, good, item), Check::Undeclared);
    item.declared_ref = declared;
    item.declared_hash = cscrypto::calculateHash(reinterpret_cast<const cs::Byte*>(good.data()), good.size());
    EXPECT_EQ(cs::SmartContracts::verify_state_reply(declared, good, item), Check::Accepted);
    EXPECT_EQ(cs::SmartContracts::verify_state_reply(declared, "a forged state", item), Check::HashMismatch);
    EXPECT_EQ(cs::SmartContracts::verify_state_reply(declared, "", item), Check::HashMismatch);
    EXPECT_EQ(cs::SmartContracts::verify_state_reply(older, good, item), Check::Older);
    EXPECT_EQ(cs::SmartContracts::verify_state_reply(newer, good, item), Check::Newer);
}

TEST(SmartContract, StateFetcherPacesAndBacksOff) {
    cs::StateFetcher f;
    std::vector<csdb::Address> contracts;
    for (uint8_t i = 1; i <= 20; ++i) {
        contracts.push_back(csdb::Address::from_public_key(cs::Bytes(32, i)));
        f.markStale(contracts.back());
    }
    EXPECT_TRUE(f.isWanted(contracts[0]));
    // at most 16 a round, the rest the next round
    EXPECT_EQ(f.due(100).size(), cs::StateFetcher::kRequestsPerRound);
    EXPECT_EQ(f.due(101).size(), 4u);
    EXPECT_TRUE(f.due(102).empty());
    // asked again after the backoff, which doubles with each attempt up to the cap
    EXPECT_EQ(f.due(100 + cs::StateFetcher::kBaseBackoff).size(), cs::StateFetcher::kRequestsPerRound);
    EXPECT_EQ(cs::StateFetcher::backoff(0), 50u);
    EXPECT_EQ(cs::StateFetcher::backoff(1), 100u);
    EXPECT_EQ(cs::StateFetcher::backoff(20), cs::StateFetcher::kMaxBackoff);
    // a verified state ends the requests, and a reply for a contract not asked for is not an answer
    f.done(contracts[0]);
    EXPECT_FALSE(f.isWanted(contracts[0]));
    EXPECT_FALSE(f.isWanted(csdb::Address::from_public_key(cs::Bytes(32, 99))));
    EXPECT_EQ(f.size(), 19u);
    f.reset();
    EXPECT_EQ(f.size(), 0u);
}
