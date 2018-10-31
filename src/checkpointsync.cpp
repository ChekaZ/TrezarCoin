// Copyright (c) 2012-2013 PPCoin developers
// Copyright (c) 2014-2017 Primecoin Developers
// Distributed under conditional MIT/X11 software license,
// see the accompanying file COPYING
//
// The synchronized checkpoint system is first developed by Sunny King for
// ppcoin network in 2012, giving cryptocurrency developers a tool to gain
// additional network protection against 51% attack.
//
// Primecoin also adopts this security mechanism, and the enforcement of
// checkpoints is explicitly granted by user, thus granting only temporary
// consensual central control to developer at the threats of 51% attack.
//
// Concepts
//
// In the network there can be a privileged node known as 'checkpoint master'.
// This node can send out checkpoint messages signed by the checkpoint master
// key. Each checkpoint is a block hash, representing a block on the blockchain
// that the network should reach consensus on.
//
// Besides verifying signatures of checkpoint messages, each node also verifies
// the consistency of the checkpoints. If a conflicting checkpoint is received,
// it means either the checkpoint master key is compromised, or there is an
// operator mistake. In this situation the node would discard the conflicting
// checkpoint message and display a warning message. This precaution controls
// the damage to network caused by operator mistake or compromised key.
//
// Operations
//
// Checkpoint master key can be established by using the 'makekeypair' command
// The public key in source code should then be updated and private key kept
// in a safe place.
//
// Any node can be turned into checkpoint master by setting the 'checkpointkey'
// configuration parameter with the private key of the checkpoint master key.
// Operator should exercise caution such that at any moment there is at most
// one node operating as checkpoint master. When switching master node, the
// recommended procedure is to shutdown the master node and restart as
// regular node, note down the current checkpoint by 'getcheckpoint', then
// compare to the checkpoint at the new node to be upgraded to master node.
// When the checkpoint on both nodes match then it is safe to switch the new
// node to checkpoint master.
//
// The configuration parameter 'checkpointdepth' specifies how many blocks
// should the checkpoints lag behind the latest block in auto checkpoint mode.
// A depth of 0 is the minimum auto checkpoint policy and offers the strongest
// protection against 51% attack. A negative depth means that the checkpoints
// should not be automatically generated by the checkpoint master, but instead
// be manually entered by operator via the 'sendcheckpoint' command. The manual
// mode is also the default mode (default value -1 for checkpointdepth).
//

#include <boost/foreach.hpp>

#include "checkpoints.h"
#include "checkpointsync.h"

#include "base58.h"
#include "main.h"
#include "txdb.h"
#include "uint256.h"
#include "txmempool.h"
#include "consensus/validation.h"
#include "consensus/consensus.h"

#include <univalue.h>

using namespace std;

// Synchronized checkpoint (centrally broadcasted)
string CSyncCheckpoint::strMasterPrivKey = "";
uint256 hashSyncCheckpoint = ArithToUint256(arith_uint256(0));
uint256 hashPendingCheckpoint = ArithToUint256(arith_uint256(0));
CSyncCheckpoint checkpointMessage;
CSyncCheckpoint checkpointMessagePending;
uint256 hashInvalidCheckpoint = ArithToUint256(arith_uint256(0));
CCriticalSection cs_hashSyncCheckpoint;
string strCheckpointWarning;

// Only descendant of current sync-checkpoint is allowed
bool ValidateSyncCheckpoint(uint256 hashCheckpoint)
{
    if (!mapBlockIndex.count(hashSyncCheckpoint))
        return error("%s: block index missing for current sync-checkpoint %s", __func__, hashSyncCheckpoint.ToString());
    if (!mapBlockIndex.count(hashCheckpoint))
        return error("%s: block index missing for received sync-checkpoint %s", __func__, hashCheckpoint.ToString());

    CBlockIndex* pindexSyncCheckpoint = mapBlockIndex[hashSyncCheckpoint];
    CBlockIndex* pindexCheckpointRecv = mapBlockIndex[hashCheckpoint];

    if (pindexCheckpointRecv->nHeight <= pindexSyncCheckpoint->nHeight)
    {
        // Received an older checkpoint, trace back from current checkpoint
        // to the same height of the received checkpoint to verify
        // that current checkpoint should be a descendant block
        if (!chainActive.Contains(pindexCheckpointRecv))
        {
            hashInvalidCheckpoint = hashCheckpoint;
            return error("%s: new sync-checkpoint %s is conflicting with current sync-checkpoint %s", __func__, hashCheckpoint.ToString(), hashSyncCheckpoint.ToString());
        }
        return false; // ignore older checkpoint
    }

    // Received checkpoint should be a descendant block of the current
    // checkpoint. Trace back to the same height of current checkpoint
    // to verify.
    CBlockIndex* pindex = pindexCheckpointRecv;
    while (pindex->nHeight > pindexSyncCheckpoint->nHeight)
        if (!(pindex = pindex->pprev))
            return error("%s: pprev2 null - block index structure failure", __func__);

    if (pindex->GetBlockHash() != hashSyncCheckpoint)
    {
        hashInvalidCheckpoint = hashCheckpoint;
        return error("%s: new sync-checkpoint %s is not a descendant of current sync-checkpoint %s", __func__, hashCheckpoint.ToString(), hashSyncCheckpoint.ToString());
    }
    return true;
}

bool WriteSyncCheckpoint(const uint256& hashCheckpoint)
{
    if (!pblocktree->WriteSyncCheckpoint(hashCheckpoint))
        return error("%s: failed to write to txdb sync checkpoint %s", __func__, hashCheckpoint.ToString());

    FlushStateToDisk();
    hashSyncCheckpoint = hashCheckpoint;
    return true;
}

bool AcceptPendingSyncCheckpoint()
{
    LOCK(cs_hashSyncCheckpoint);
    bool havePendingCheckpoint = hashPendingCheckpoint != ArithToUint256(arith_uint256(0)) && mapBlockIndex.count(hashPendingCheckpoint);
    if (!havePendingCheckpoint)
        return false;

    if (!ValidateSyncCheckpoint(hashPendingCheckpoint))
    {
        hashPendingCheckpoint = ArithToUint256(arith_uint256(0));
        checkpointMessagePending.SetNull();
        return false;
    }

    if (!chainActive.Contains(mapBlockIndex[hashPendingCheckpoint]))
        return false;

    if (!WriteSyncCheckpoint(hashPendingCheckpoint))
        return error("%s: failed to write sync checkpoint %s", __func__, hashPendingCheckpoint.ToString());

    hashPendingCheckpoint = ArithToUint256(arith_uint256(0));
    checkpointMessage = checkpointMessagePending;
    checkpointMessagePending.SetNull();

    // Relay the checkpoint
    if (!checkpointMessage.IsNull())
    {
        BOOST_FOREACH(CNode* pnode, vNodes)
            checkpointMessage.RelayTo(pnode);
    }
    return true;
}

// Automatically select a suitable sync-checkpoint
uint256 AutoSelectSyncCheckpoint()
{
    // Search backward for a block with specified depth policy
    const CBlockIndex *pindex = chainActive.Tip();
    while (pindex->pprev && pindex->nHeight + (int)GetArg("-checkpointdepth", -1) > chainActive.Tip()->nHeight)
        pindex = pindex->pprev;
    return pindex->GetBlockHash();
}

// Check against synchronized checkpoint
bool CheckSyncCheckpoint(const uint256& hashBlock, const CBlockIndex* pindexPrev)
{
    int nHeight = pindexPrev->nHeight + 1;

    LOCK(cs_hashSyncCheckpoint);
    // Reset checkpoint to Genesis block if not found or initialised
    if (hashSyncCheckpoint == ArithToUint256(arith_uint256(0)) || !(mapBlockIndex.count(hashSyncCheckpoint))) {
        WriteSyncCheckpoint(Params().GetConsensus().hashGenesisBlock);
        return true;
    }
    const CBlockIndex* pindexSync = mapBlockIndex[hashSyncCheckpoint];

    if (nHeight > pindexSync->nHeight)
    {
        // Trace back to same height as sync-checkpoint
        const CBlockIndex* pindex = pindexPrev;
        while (pindex->nHeight > pindexSync->nHeight)
            if (!(pindex = pindex->pprev))
                return error("%s: pprev null - block index structure failure", __func__);

        // at this point we could have:
        // 1. found block in our blockchain
        // 2. reached pindexSync->nHeight without finding it
        if (!chainActive.Contains(pindex))
            return false; // only descendant of sync-checkpoint can pass check
    }
    if (nHeight == pindexSync->nHeight && hashBlock != hashSyncCheckpoint)
        return false; // Same height with sync-checkpoint
    if (nHeight < pindexSync->nHeight && !mapBlockIndex.count(hashBlock))
        return false; // Lower height than sync-checkpoint
    return true;
}

// Reset synchronized checkpoint to last hardened checkpoint
bool ResetSyncCheckpoint()
{
    LOCK(cs_hashSyncCheckpoint);

    // Hash of latest checkpoint
    uint256 checkpointHash = Checkpoints::GetLatestHardenedCheckpoint(Params().Checkpoints());

    // Checkpoint block not yet accepted
    if (!mapBlockIndex.count(checkpointHash)) {
        checkpointMessagePending.SetNull();
        hashPendingCheckpoint = checkpointHash;
    }

    if (!WriteSyncCheckpoint((mapBlockIndex.count(checkpointHash) && chainActive.Contains(mapBlockIndex[checkpointHash]))? checkpointHash : Params().GetConsensus().hashGenesisBlock))
        return error("%s: failed to write sync checkpoint %s", __func__, checkpointHash.ToString());

    return true;
}

void AskForPendingSyncCheckpoint(CNode* pfrom)
{
    LOCK(cs_hashSyncCheckpoint);
    if (pfrom && hashPendingCheckpoint != ArithToUint256(arith_uint256(0)) && !mapBlockIndex.count(hashPendingCheckpoint))
        pfrom->AskFor(CInv(MSG_BLOCK, hashPendingCheckpoint));
}

// Verify sync checkpoint master pubkey and reset sync checkpoint if changed
bool CheckCheckpointPubKey()
{
    string strPubKey = "";
    string strMasterPubKey = Params().GetConsensus().checkpointPubKey;

    if (!pblocktree->ReadCheckpointPubKey(strPubKey) || strPubKey != strMasterPubKey)
    {
        // write checkpoint master key to db
        if (!pblocktree->WriteCheckpointPubKey(strMasterPubKey))
            return error("%s: failed to write new checkpoint master key to db", __func__);
        if (!pblocktree->Sync())
            return error("%s: failed to commit new checkpoint master key to db", __func__);
        if (!ResetSyncCheckpoint())
            return error("%s: failed to reset sync-checkpoint", __func__);
    }

    return true;
}

bool SetCheckpointPrivKey(string strPrivKey)
{
    CBitcoinSecret vchSecret;
    if (!vchSecret.SetString(strPrivKey))
        return error("%s: Checkpoint master key invalid", __func__);

    CKey key = vchSecret.GetKey();
    if (!key.IsValid())
        return false;

    CSyncCheckpoint::strMasterPrivKey = strPrivKey;
    return true;
}

bool SendSyncCheckpoint(uint256 hashCheckpoint)
{
    CSyncCheckpoint checkpoint;
    checkpoint.hashCheckpoint = hashCheckpoint;
    CDataStream sMsg(SER_NETWORK, PROTOCOL_VERSION);
    sMsg << (CUnsignedSyncCheckpoint)checkpoint;
    checkpoint.vchMsg = vector<unsigned char>(sMsg.begin(), sMsg.end());

    if (CSyncCheckpoint::strMasterPrivKey.empty())
        return error("%s: Checkpoint master key unavailable.", __func__);

    CBitcoinSecret vchSecret;
    if (!vchSecret.SetString(CSyncCheckpoint::strMasterPrivKey))
        return error("%s: Checkpoint master key invalid", __func__);

    CKey key = vchSecret.GetKey(); // If key is not correct openssl may crash
    if (!key.Sign(Hash(checkpoint.vchMsg.begin(), checkpoint.vchMsg.end()), checkpoint.vchSig))
        return error("%s: Unable to sign checkpoint, check private key?", __func__);

    if (!checkpoint.ProcessSyncCheckpoint(NULL))
        return error("%s: Failed to process checkpoint.", __func__);

    // Relay checkpoint
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
            checkpoint.RelayTo(pnode);
    }
    return true;
}

// Verify signature of sync-checkpoint message
bool CSyncCheckpoint::CheckSignature()
{
    string strMasterPubKey = Params().GetConsensus().checkpointPubKey;
    CPubKey key(ParseHex(strMasterPubKey));
    if (!key.Verify(Hash(vchMsg.begin(), vchMsg.end()), vchSig))
        return error("%s: verify signature failed", __func__);

    // Now unserialize the data
    CDataStream sMsg(vchMsg, SER_NETWORK, PROTOCOL_VERSION);
    sMsg >> *(CUnsignedSyncCheckpoint*)this;
    return true;
}

// Process synchronized checkpoint
bool CSyncCheckpoint::ProcessSyncCheckpoint(CNode* pfrom)
{
    if (!CheckSignature())
        return false;

    LOCK(cs_hashSyncCheckpoint);
    if (!mapBlockIndex.count(hashCheckpoint))
    {
        // We haven't received the checkpoint chain, keep the checkpoint as pending
        hashPendingCheckpoint = hashCheckpoint;
        checkpointMessagePending = *this;

        return false;
    }

    if (!ValidateSyncCheckpoint(hashCheckpoint))
        return false;

    if (!WriteSyncCheckpoint(hashCheckpoint))
        return error("%s: failed to write sync checkpoint %s", __func__, hashCheckpoint.ToString());

    checkpointMessage = *this;
    hashPendingCheckpoint = ArithToUint256(arith_uint256(0));
    checkpointMessagePending.SetNull();

    return true;
}
