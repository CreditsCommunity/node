#define TESTING

#include <solver/smartcontracts.hpp>
#include <solver/statefetcher.hpp>
#include "clientconfigmock.hpp"
#include "gtest/gtest.h"
#include <csdb/transaction.hpp>
#include <apihandler.hpp>
#include <base58.h>
#include <serializer.hpp>
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

// the execution an emitted transaction is charged to, as on_next_block_impl decides it: the rule's
// answer, or the contract's execution before the block when no successful new_state precedes it
cs::SmartContractRef charged(const csdb::Pool& block, size_t i, const csdb::Address& contract, const cs::SmartContractRef& before) {
    return cs::SmartContracts::emitting_new_state(block, i, contract, sameAddress).value_or(before);
}

// the same refund as on_remove_block computes it, walking the block backwards
std::vector<std::pair<size_t, cs::SmartContractRef>> refunded(const csdb::Pool& block, const csdb::Address& contract, const cs::SmartContractRef& before) {
    std::vector<std::pair<size_t, cs::SmartContractRef>> result;
    for (auto itr_t = block.transactions().crbegin(); itr_t != block.transactions().crend(); ++itr_t) {
        if (cs::SmartContracts::is_new_state(*itr_t) || itr_t->source() != contract) {
            continue;
        }
        const size_t tr_idx = static_cast<size_t>(std::distance(itr_t, block.transactions().crend())) - 1;
        result.emplace_back(tr_idx, charged(block, tr_idx, contract, before));
    }
    return result;
}

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
    EXPECT_EQ(charged(other, 1, A, refOf(46, 0)), refOf(46, 0));
}

// The official node charges an emitted transaction to the contract's ref_execute. A node reading the
// block has advanced it in block order; a live node has applied every state of the block before storing
// it (statesCreated -> on_update) and holds the latest. on_next_block_impl reads the block instead, and
// its fall-back takes the execution on_update saved before applying the states, not ref_execute: the
// two paths charge the same executions, the ones a reading node holds.
TEST(SmartContract, EmittedTransactionChargeIsTheSameLiveAndReplaying) {
    Block25897074 base;
    const auto& dice = base.dice;
    csdb::Pool block(csdb::PoolHash{}, 25897074);
    block.add_transaction(plain(dice, base.player, 7));   // 0: a payout no new_state precedes
    for (const auto& t : base.block.transactions()) {
        block.add_transaction(t);
    }
    const auto before = refOf(25896000, 0);   // the contract's execution before the block

    // on_next_block_impl: the rule, or the saved execution, or ref_execute when nothing was saved
    const auto node_charge = [&](size_t i, const cs::SmartContractRef& ref_execute, const std::optional<cs::SmartContractRef>& saved) {
        return cs::SmartContracts::emitting_new_state(block, i, dice, sameAddress).value_or(saved.value_or(ref_execute));
    };
    const cs::SmartContractRef live_ref_execute = refOf(25897070, 1);   // after on_update
    cs::SmartContractRef replay_ref_execute = before;
    size_t payouts = 0;
    for (size_t i = 0; i < block.transactions().size(); ++i) {
        const auto& t = block.transactions()[i];
        if (cs::SmartContracts::is_new_state(t)) {
            if (cs::SmartContracts::is_state_updated(t)) {
                replay_ref_execute = cs::SmartContractRef(t.user_field(cs::trx_uf::new_state::RefStart));
            }
            continue;
        }
        if (t.source() != dice) {
            continue;
        }
        ++payouts;
        const auto replaying = node_charge(i, replay_ref_execute, std::nullopt);
        const auto live = node_charge(i, live_ref_execute, before);
        EXPECT_EQ(replaying, live) << i;
        EXPECT_EQ(replaying, replay_ref_execute) << i;   // what the official rule charges when reading
        if (i == 4) {
            // the official ambiguity: reading charges 25,897,070.0, live 25,897,070.1
            EXPECT_FALSE(replay_ref_execute == live_ref_execute);
        }
    }
    EXPECT_EQ(payouts, 3u);
}

// A rollback refunds each emitted transaction to the execution it was charged to: on_remove_block
// walks the block backwards and applies the same rule at the same position.
TEST(SmartContract, RollbackRefundsEmittedTransactionsToTheChargedExecution) {
    Block25897074 base;
    csdb::Pool block(csdb::PoolHash{}, 25897074);
    block.add_transaction(newState(base.dice, 25897069, 0, 7, '\0'));   // 0: a failed execution
    block.add_transaction(plain(base.dice, base.player, 8));             // 1: a payout no successful new_state precedes
    for (const auto& t : base.block.transactions()) {
        block.add_transaction(t);
    }
    const auto before = refOf(25896000, 0);
    const auto refunds = refunded(block, base.dice, before);
    ASSERT_EQ(refunds.size(), 3u);
    for (const auto& [i, ref] : refunds) {
        EXPECT_EQ(ref, charged(block, i, base.dice, before)) << i;
    }
    // backwards: GZNHaxfo's payout to 25,897,070.1, 2bAyU7bF's to 25,897,070.0, the first to the
    // execution before the block
    EXPECT_EQ(refunds[0].first, 7u);
    EXPECT_EQ(refunds[0].second, refOf(25897070, 1));
    EXPECT_EQ(refunds[1].first, 5u);
    EXPECT_EQ(refunds[1].second, refOf(25897070, 0));
    EXPECT_EQ(refunds[2].first, 1u);
    EXPECT_EQ(refunds[2].second, before);
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
