#include <consensus.hpp>
#include <smartcontracts.hpp>
#include <solvercontext.hpp>
#include <states/trustedstage1state.hpp>

#include <csdb/amount.hpp>
#include <csnode/blockchain.hpp>
#include <csnode/conveyer.hpp>
#include <csnode/itervalidator.hpp>
#include <csnode/transactionspacket.hpp>
#include <csnode/walletscache.hpp>
#include <csnode/configholder.hpp>
#include <lib/system/logger.hpp>
#include <lib/system/utils.hpp>

#include <csnode/datastream.hpp>
#include <cscrypto/cscrypto.hpp>
#include <algorithm>
#include <string>

namespace cs {
void TrustedStage1State::on(SolverContext& context) {
    if (!pValidator_) {
        pValidator_ = std::make_unique<IterValidator>(context.wallets());
    }
    likeMineHashes = 0;
    differentHashes = 0;
    differKeys.clear();
    DefaultStateBehavior::on(context);
    context.init_zero(stage);
    stage.sender = context.own_conf_number();
    enough_hashes = false;
    transactions_checked = false;
    min_time_expired = false;

    SolverContext* pctx = &context;
    auto dt = Consensus::TimeMinStage1;
    csdebug() << name() << ": start track min time " << dt << " ms to get hashes";

    cs::Timer::singleShot(dt, cs::RunPolicy::CallQueuePolicy, [this, pctx]() {
        csdebug() << name() << ": min time to get hashes is expired, may proceed to the next state, hashes enough: " 
            << (enough_hashes ? "Ok" : "NOT Ok") << ", transactions checked: " << (transactions_checked ? "Ok" : "NOT Ok");
        min_time_expired = true;
        if (transactions_checked && enough_hashes) {
            csdebug() << name() << ": transactions & hashes ready, so proceed to the next state now";
            pctx->complete_stage1();
        }
        else {
            csdebug() << name() << ": Something prevents from finishing Stage-1, perhaps later...";
        }
    });

    // min_time_tracking.start(
    //  context.scheduler(), dt,
    //  [this, pctx](){
    //    csdebug() << name() << ": min time to get hashes is expired, may proceed to the next state";
    //    min_time_expired = true;
    //    if (transactions_checked && enough_hashes) {
    //      csdebug() << name() << ": transactions & hashes ready, so proceed to the next state now";
    //      pctx->complete_stage1();
    //    }
    //  },
    //  true /*replace if exists*/
    //);
}

void TrustedStage1State::finalizeStage(SolverContext& context) {
    
    //if(context.own_conf_number() == 1 && cs::Conveyer::instance().currentRoundNumber() > 10) {
    //    stage.roundTimeStamp = std::to_string(cs::Utils::currentTimestamp() + 10000ULL); 
    //} 
    //else {
    uint64_t lastTimeStamp = 0;
    try {
        lastTimeStamp = std::stoull(context.blockchain().getLastTimeStamp().c_str());
    }
    catch (...) {
        csdebug() << name() << __func__ << ": blockchain tmestamp was announced as zero";
    }
    uint64_t currentTimeStamp = cs::Utils::currentTimestamp();
    if (currentTimeStamp < lastTimeStamp) {
        currentTimeStamp = lastTimeStamp + 1;
    }
    stage.roundTimeStamp = std::to_string(currentTimeStamp);
        /*}*/
    stage.toBytes();
    stage.messageHash = cscrypto::calculateHash(stage.message.data(), stage.message.size());
    cs::Bytes messageToSign;
    messageToSign.reserve(sizeof(cs::RoundNumber) + sizeof(uint8_t) + sizeof(cs::Hash));
    cs::ODataStream signStream(messageToSign);
    signStream << cs::Conveyer::instance().currentRoundNumber();
    signStream << context.subRound();
    signStream << stage.messageHash;
    stage.signature = cscrypto::generateSignature(context.private_key(), messageToSign.data(), messageToSign.size());
}

void TrustedStage1State::off(SolverContext& context) {
    // if (min_time_tracking.cancel()) {
    //  csdebug() << name() << ": cancel track min time to get hashes";
    //}
    csdebug() << name() << ": --> stage-1 [" << static_cast<int>(stage.sender) << "]";
    if (min_time_expired && transactions_checked && enough_hashes) {
        // a stage 1 already signed and sent for this round and sub-round: the round was entered
        // again after a fuller round package replaced the first one (SolverCore::nextRound(true))
        const cs::StageOne* sent = context.sent_stage1();
        std::string sentStamp;
        std::string differs;
        if (sent != nullptr) {
            sentStamp = sent->roundTimeStamp;
            differs = stage1ContentDiff(*sent, stage);
            // Never sign a second stage 1 for the same round and sub-round: send the same signed bytes
            // again, as gotStageOneRequest() does. A second signed message of the seat makes the
            // confidants hold different stage-1 copies (a receiver keeps the later stamp only if it
            // gets both): they choose different block timestamps, or see two hash-signature pairs of
            // this seat at stage 2 and exclude it. Receivers verify the message as any other; one
            // that already holds it ignores it (same timestamp).
            // What the stored message may lack is harmless: the trusted candidates and the packet
            // hashes are this node's votes for the next round, and the hashes or packets that came in
            // after the first stage 1 only miss one vote, as if the replacement had not happened;
            // the others already hold and accept that message. The characteristic hash is different:
            // it must match the majority or this seat is marked a liar, so when it changed (the
            // replaced package changed the last block and so the wallets the transactions are
            // checked against), the new stage 1 is signed as before. So is a stored timestamp no
            // longer after the last block's, which the receivers would refuse.
            uint64_t lastStamp = 0;
            uint64_t sentStampValue = 0;
            try {
                lastStamp = std::stoull(context.blockchain().getLastTimeStamp());
                sentStampValue = std::stoull(sent->roundTimeStamp);
            }
            catch (...) {
                sentStampValue = 0;
            }
            if (sent->hash == stage.hash && sentStampValue > lastStamp) {
                stage = *sent;
                cslog() << name() << ": stage1 resent unchanged: R-" << cs::Conveyer::instance().currentRoundNumber() << "."
                        << static_cast<int>(context.subRound()) << " T" << static_cast<int>(stage.sender) << " ts " << sentStamp
                        << (differs.empty() ? std::string{} : ", a new one would differ in " + differs);
                context.add_stage1(stage, true);
                return;
            }
        }
        finalizeStage(context);
        if (sent != nullptr) {
            // diagnostics only: the receivers now hold two signed stage-1 messages of this seat
            cslog() << name() << ": stage1 signed again: R-" << cs::Conveyer::instance().currentRoundNumber() << "."
                    << static_cast<int>(context.subRound()) << " T" << static_cast<int>(stage.sender) << " ts " << sentStamp
                    << " -> " << stage.roundTimeStamp << ", content " << (differs.empty() ? "same" : "differs (" + differs + ")");
        }
        context.add_stage1(stage, true);
        context.remember_sent_stage1(stage);
    }
}

// the fields of two stage-1 messages of one seat that differ, timestamp and signature aside
// (empty: the same content); candidates compare as sets, their order is the arrival order
std::string TrustedStage1State::stage1ContentDiff(const cs::StageOne& a, const cs::StageOne& b) {
    std::string diff;
    auto add = [&diff](const char* field) {
        diff += diff.empty() ? field : std::string(",") + field;
    };
    if (a.sender != b.sender) {
        add("sender");
    }
    if (a.hash != b.hash) {
        add("hash");
    }
    auto sameSet = [](auto x, auto y) {
        std::sort(x.begin(), x.end());
        std::sort(y.begin(), y.end());
        return x == y;
    };
    if (!sameSet(a.trustedCandidates, b.trustedCandidates)) {
        add("candidates");
    }
    if (a.hashesCandidates != b.hashesCandidates) {
        add("packets");
    }
    return diff;
}

Result TrustedStage1State::onSyncTransactions(SolverContext& context, cs::RoundNumber round) {
    cs::Conveyer& conveyer = cs::Conveyer::instance();
    if (round < conveyer.currentRoundNumber()) {
        cserror() << name() << ": cannot handle transactions from old round " << round;
        return Result::Ignore;
    }

    csdebug() << name() << ": -------> STARTING CONSENSUS #" << conveyer.currentRoundNumber() << " <------- ";
    auto data = conveyer.createPacket(round);

    if (!data.has_value()) {
        cserror() << name() << ": error while prepare consensus to build vector, maybe method called before sync completed?";
        return Result::Ignore;
    }

    // bindings
    auto&& [packet, smartContractPackets] = std::move(data).value();

    csdebug() << name() << ": packet of " << packet.transactionsCount() << " transactions in" << typeid(conveyer).name();
    if (!smartContractPackets.empty()) {
        csdebug() << name() << ": smart contract packets count " << smartContractPackets.size();
        if (!smartContractPackets.empty()) {
            for (const auto& p : smartContractPackets) {
                csdetails() << name() << ": packet hash " << p.hash().toString();
            }
        }
    }

    // review & validate transactions
    stage.hash = build_vector(context, packet, smartContractPackets);

    {
        std::unique_lock<cs::SharedMutex> lock = conveyer.lock();
        size_t trxCounter = 0;
        size_t preliminaryBlockSize = 0;
        size_t deltaBlockSize = 0;
        const cs::RoundTable& roundTable = conveyer.currentRoundTable();
        bool finishFlag = false;
        bool continueFlag = false;
        size_t tSize = 0;
        for (const auto& element : conveyer.transactionsPacketTable()) {
            if (conveyer.currentRoundNumber() + 2 >= element.second.expiredRound()) {
                continue;
            }

            const cs::PacketsHashes& hashes = roundTable.hashes;
            continueFlag = false;
            if (std::find(hashes.cbegin(), hashes.cend(), element.first) == hashes.cend()) {
                
                if (stage.hashesCandidates.size() > Consensus::MaxStageOneHashes) {
                    finishFlag = true;
                }

                trxCounter += element.second.transactionsCount();
                if (trxCounter > Consensus::MaxStageOneTransactions) {
                    finishFlag = true;
                }

                deltaBlockSize = 0;
                for (auto& it : element.second.transactions()) {
                    tSize = it.to_byte_stream().size();
                    deltaBlockSize += tSize;
                    preliminaryBlockSize += tSize;
                    csdetails() << name() << ": include transaction " << tSize << " bytes";
                    if (preliminaryBlockSize > Consensus::MaxPreliminaryBlockSize) {
                        finishFlag = true;
                    }
                    if (tSize > Consensus::MaxTransactionSize) {
                        continueFlag = true;
                    }

                }

                if (finishFlag) {
                    trxCounter -= element.second.transactionsCount();
                    preliminaryBlockSize -= deltaBlockSize;
                    break;
                }
                if (continueFlag) {
                    trxCounter -= element.second.transactionsCount();
                    preliminaryBlockSize -= deltaBlockSize;
                    continue;
                }

                stage.hashesCandidates.push_back(element.first);
            }
        }
        csdebug() << name() << ": transactions: " << trxCounter << ", preliminary block size: " << preliminaryBlockSize;
    }

    transactions_checked = true;
    bool other_conditions = enough_hashes && min_time_expired;
    return (other_conditions ? Result::Finish : Result::Ignore);
}

Result TrustedStage1State::onHash(SolverContext& context, const csdb::PoolHash& pool_hash, const cs::PublicKey& sender) {
    csdb::PoolHash lastHash = context.blockchain().getLastHash();
    csdb::PoolHash spoiledHash = context.spoileHash(lastHash, sender);
    std::string stagesPlot;
    std::string hString = std::string(name()) + ": <-- hash from " + std::string(context.sender_description(sender));
    if (spoiledHash == pool_hash) {
        // get node status for useful logging
        if (cs::Conveyer::instance().isConfidantExists(sender)) {
            ++likeMineHashes;
        }
        
        if (std::find(stage.trustedCandidates.cbegin(), stage.trustedCandidates.cend(), sender) == stage.trustedCandidates.cend()) {
            stage.trustedCandidates.push_back(sender);
            csdebug() << hString << ": hash is OK: added, now total: " << stage.trustedCandidates.size();
        }
        else {
            csdebug() << hString << ": hash is OK, but dupplicated: not added remains: " << stage.trustedCandidates.size();
        }
        // candidates does not include own hash, so (MinTrustedNodes - 1) will be enough to proceed next stage
        if (stage.trustedCandidates.size() >= Consensus::MinTrustedNodes - 1) {
            //if (likeMineHashes >= cs::Conveyer::instance().confidantsCount() / 2) {
            enough_hashes = true;
            bool other_conditions = transactions_checked && min_time_expired;
            return (other_conditions ? Result::Finish : Result::Ignore);
            //}
            //else if(differentHashes >= cs::Conveyer::instance().confidantsCount() / 2){
            //    context.askTrustedRound(cs::Conveyer::instance().currentRoundNumber(), differKeys);
            //}
        }
    }
    else {
        csdebug() << hString << ": DOES NOT MATCH my value " << lastHash.to_string();
        //TODO: print hashMask string
        if (cs::Conveyer::instance().isConfidantExists(sender)) {
            ++differentHashes;
            differKeys.push_back(sender);
        }
        else {
            context.sendHashReply(std::move(pool_hash), sender);
        }
        if (differentHashes >= cs::Conveyer::instance().confidantsCount() / 2) {
            context.askTrustedRound(cs::Conveyer::instance().currentRoundNumber(), differKeys);
        }

    }

    return Result::Ignore;
}

cs::Hash TrustedStage1State::build_vector(SolverContext& context, cs::TransactionsPacket& packet, cs::PacketsVector& smartsPackets) {
    const std::size_t transactionsCount = packet.transactionsCount();

    cs::Characteristic characteristic;

    if (transactionsCount > 0) {
        characteristic = pValidator_->formCharacteristic(context, packet.transactions(), smartsPackets);
    }

    if (characteristic.mask.size() != transactionsCount) {
        cserror() << name() << ": characteristic mask size is not equal to transactions count in build_vector()";
    }

    if (std::find_if(characteristic.mask.cbegin(), characteristic.mask.cend(), [](const cs::Byte& item) { return item != Reject::Reason::None; }) != characteristic.mask.cend()) {
        context.send_rejected_report(characteristic.mask);
    }

    // transform characteristic to its "canonical" form
    pValidator_->normalizeCharacteristic(characteristic);

    cs::Conveyer& conveyer = cs::Conveyer::instance();
    conveyer.setCharacteristic(characteristic, conveyer.currentRoundNumber());

    return formHashFromCharacteristic(characteristic);
}

cs::Hash TrustedStage1State::formHashFromCharacteristic(const cs::Characteristic& characteristic) {
    cs::Hash hash;

    if (characteristic.mask.empty()) {
        cs::Conveyer& conveyer = cs::Conveyer::instance();
        auto round = conveyer.currentRoundNumber();
        hash = cscrypto::calculateHash(reinterpret_cast<cs::Byte*>(&round), sizeof(cs::RoundNumber));
    }
    else {
        hash = cscrypto::calculateHash(characteristic.mask.data(), characteristic.mask.size());
    }

    csdebug() << name() << ": generated hash: " << cs::Utils::byteStreamToHex(hash.data(), hash.size());
    return hash;
}
}  // namespace cs
