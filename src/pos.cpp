// Copyright (c) 2017-2018 The DeepOnion developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp>

#include "pos.h"
#include "txdb.h"
#include "arith_uint256.h"

using namespace std;

typedef std::map<int, unsigned int> MapModifierCheckpoints;

unsigned int nStakeMinAge = 60 * 60 * 24 * 1;			// minimum age for coin age: 1d
unsigned int nStakeMaxAge = 60 * 60 * 24 * 30;	        // stake age of full weight: 30d
unsigned int nModifierInterval = 8 * 60;				// time to elapse before new modifier is computed

// Hard checkpoints of stake modifiers to ensure they are deterministic
static std::map<int, unsigned int> mapStakeModifierCheckpoints =
    boost::assign::map_list_of
    (     0, 0xfd11f4e7u)
    (  1000, 0x353653feu)
    ( 10000, 0x8c341084u)
    ( 50008, 0x9f0053f2u)
    (100000, 0xaf212909u)
    (150006, 0x3883af95u)
    (200830, 0xf2daec0au)
    (250008, 0x76bd1777u)
    (300836, 0x18dbac5eu)
    (350003, 0x17223fa8u)
    (400002, 0xd1662b8fu)
    (450000, 0x0fc0c8d3u)
    (500001, 0x17ac1811u)
    (550004, 0xcfb3340fu)
    (600014, 0x74d7cf8cu)
    (621306, 0x4890a081u)
	;

// Hard checkpoints of stake modifiers to ensure they are deterministic (testNet)
static std::map<int, unsigned int> mapStakeModifierCheckpointsTestNet =
    boost::assign::map_list_of
    ( 0, 0xfd11f4e7u )
    ;

// Get time weight
int64_t GetWeight(int64_t nIntervalBeginning, int64_t nIntervalEnd)
{
    // Kernel hash weight starts from 0 at the min age
    // this change increases active coins participating the hash and helps
    // to secure the network when proof-of-stake difficulty is low

    return min(nIntervalEnd - nIntervalBeginning - nStakeMinAge, (int64_t)nStakeMaxAge);
}

// Get the last stake modifier and its generation time from a given block
static bool GetLastStakeModifier(const CBlockIndex* pindex, uint64_t& nStakeModifier, int64_t& nModifierTime)
{
    if (!pindex)
        return error("GetLastStakeModifier: null pindex");
    while (pindex && pindex->pprev && !pindex->GeneratedStakeModifier())
        pindex = pindex->pprev;
    if (!pindex->GeneratedStakeModifier())
        return error("GetLastStakeModifier: no generation at genesis block");
    nStakeModifier = pindex->nStakeModifier;
    nModifierTime = pindex->GetBlockTime();
    return true;
}

// Get selection interval section (in seconds)
static int64_t GetStakeModifierSelectionIntervalSection(int nSection)
{
    assert (nSection >= 0 && nSection < 64);
    return (nModifierInterval * 63 / (63 + ((63 - nSection) * (MODIFIER_INTERVAL_RATIO - 1))));
}

// Get stake modifier selection interval (in seconds)
static int64_t GetStakeModifierSelectionInterval()
{
    int64_t nSelectionInterval = 0;
    for (int nSection=0; nSection<64; nSection++)
        nSelectionInterval += GetStakeModifierSelectionIntervalSection(nSection);
    return nSelectionInterval;
}

// select a block from the candidate blocks in vSortedByTimestamp, excluding
// already selected blocks in vSelectedBlocks, and with timestamp up to
// nSelectionIntervalStop.
static bool SelectBlockFromCandidates(vector<pair<int64_t, uint256> >& vSortedByTimestamp, map<uint256, const CBlockIndex*>& mapSelectedBlocks,
    int64_t nSelectionIntervalStop, uint64_t nStakeModifierPrev, const CBlockIndex** pindexSelected)
{
    bool fSelected = false;
    arith_uint256 hashBest = arith_uint256();
    *pindexSelected = (const CBlockIndex*) 0;

    for(const pair<int64_t, uint256>& item : vSortedByTimestamp)
    {
        if (!mapBlockIndex.count(item.second))
            return error("SelectBlockFromCandidates: failed to find block index for candidate block %s", item.second.ToString().c_str());
        const CBlockIndex* pindex = mapBlockIndex[item.second];
        if (fSelected && pindex->GetBlockTime() > nSelectionIntervalStop)
            break;
        if (mapSelectedBlocks.count(pindex->GetBlockHash()) > 0)
        {
            continue;
        }
        // compute the selection hash by hashing its proof-hash and the
        // previous proof-of-stake modifier
        uint256 hashProof = pindex->IsProofOfStake()? pindex->hashProofOfStake : pindex->GetBlockHash();
        CDataStream ss(SER_GETHASH, 0);
        ss << hashProof << nStakeModifierPrev;
        arith_uint256 hashSelection = UintToArith256(Hash(ss.begin(), ss.end()));

        // the selection hash is divided by 2**32 so that proof-of-stake block
        // is always favored over proof-of-work block. this is to preserve
        // the energy efficiency property
        if (pindex->IsProofOfStake())
            hashSelection >>= 32;
        if (fSelected && hashSelection < hashBest)
        {
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
        else if (!fSelected)
        {
            fSelected = true;
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
    }

    return fSelected;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
// Stake modifier consists of bits each of which is contributed from a
// selected block of a given block group in the past.
// The selection of a block is based on a hash of the block's proof-hash and
// the previous stake modifier.
// Stake modifier is recomputed at a fixed time interval instead of every 
// block. This is to make it difficult for an attacker to gain control of
// additional bits in the stake modifier, even after generating a chain of
// blocks.
bool ComputeNextStakeModifier(const CBlockIndex* pindexPrev, uint64_t& nStakeModifier, bool& fGeneratedStakeModifier, const Consensus::Params& consensusParams)
{
    nStakeModifier = 0;
    fGeneratedStakeModifier = false;
    if (!pindexPrev)
    {
        fGeneratedStakeModifier = true;
        return true;  // genesis block's modifier is 0
    }
    // First find current stake modifier and its generation block time
    // if it's not old enough, return the same stake modifier
    int64_t nModifierTime = 0;
    if (!GetLastStakeModifier(pindexPrev, nStakeModifier, nModifierTime))
        return error("ComputeNextStakeModifier: unable to get last modifier");

    std::string timeStr0 = DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nModifierTime);
    LogPrintf("Compute: prev modifier=0x%016x, time=%s\n", nStakeModifier, timeStr0.c_str());
    if (nModifierTime / nModifierInterval >= pindexPrev->GetBlockTime() / nModifierInterval)
        return true;

    // Sort candidate blocks by timestamp
    vector<pair<int64_t, uint256> > vSortedByTimestamp;
    vSortedByTimestamp.reserve(64 * nModifierInterval / consensusParams.nPosTargetSpacing);
    int64_t nSelectionInterval = GetStakeModifierSelectionInterval();
    int64_t nSelectionIntervalStart = (pindexPrev->GetBlockTime() / nModifierInterval) * nModifierInterval - nSelectionInterval;
    const CBlockIndex* pindex = pindexPrev;
    while (pindex && pindex->GetBlockTime() >= nSelectionIntervalStart)
    {
        vSortedByTimestamp.push_back(make_pair(pindex->GetBlockTime(), pindex->GetBlockHash()));
        pindex = pindex->pprev;
    }
    int nHeightFirstCandidate = pindex ? (pindex->nHeight + 1) : 0;
    reverse(vSortedByTimestamp.begin(), vSortedByTimestamp.end());
    sort(vSortedByTimestamp.begin(), vSortedByTimestamp.end(), [](const pair<int64_t, uint256> &a, const pair<int64_t, uint256> &b) {
        return a.first < b.first;
    });

    // Select 64 blocks from candidate blocks to generate stake modifier
    uint64_t nStakeModifierNew = 0;
    int64_t nSelectionIntervalStop = nSelectionIntervalStart;
    map<uint256, const CBlockIndex*> mapSelectedBlocks;
    for (int nRound=0; nRound<min(64, (int)vSortedByTimestamp.size()); nRound++)
    {
        // add an interval section to the current selection round
        nSelectionIntervalStop += GetStakeModifierSelectionIntervalSection(nRound);
        // select a block from the candidates of current round
        if (!SelectBlockFromCandidates(vSortedByTimestamp, mapSelectedBlocks, nSelectionIntervalStop, nStakeModifier, &pindex))
            return error("ComputeNextStakeModifier: unable to select block at round %d", nRound);
        // write the entropy bit of the selected block
        nStakeModifierNew |= (((uint64_t)pindex->GetStakeEntropyBit()) << nRound);
        // add the selected block from candidates to selected list
        mapSelectedBlocks.insert(make_pair(pindex->GetBlockHash(), pindex));
        // LogPrintf("Compute: selected round %d stop=%s height=%d bit=%d\n", nRound, DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nSelectionIntervalStop).c_str(), pindex->nHeight, pindex->GetStakeEntropyBit());
    }

    LogPrintf("Compute: new modifier=0x%016x, time=%s\n", nStakeModifierNew, DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pindexPrev->GetBlockTime()).c_str());

    nStakeModifier = nStakeModifierNew;
    fGeneratedStakeModifier = true;
    return true;
}

// The stake modifier used to hash for a stake kernel is chosen as the stake
// modifier about a selection interval later than the coin generating the kernel
static bool GetKernelStakeModifier(CBlockIndex* pindexFrom, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime)
{
    nStakeModifier = 0;
    nStakeModifierHeight = pindexFrom->nHeight;
    nStakeModifierTime = pindexFrom->nTime;
    int64_t nStakeModifierSelectionInterval = GetStakeModifierSelectionInterval();
    
    // LogPrintf(">> GetKernelStakeModifier(): nStakeModifierHeight = %d, nStakeModifierTime = %s, nStakeModifierSelectionInterval = %ll\n\n", 
    //		nStakeModifierHeight, DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nStakeModifierTime).c_str(), nStakeModifierSelectionInterval);

    const CBlockIndex* pindex = pindexFrom;
    // loop to find the stake modifier later by a selection interval
    while (nStakeModifierTime < pindexFrom->nTime + nStakeModifierSelectionInterval)
    {    	
        if (pindex->pnext == nullptr)
        {   // reached best block; may happen if node is behind on block chain
            if (pindex->GetBlockTime() + nStakeMinAge - nStakeModifierSelectionInterval > GetAdjustedTime())
                return error("GetKernelStakeModifier() : reached best block %s at height %d from block %s",
                    pindex->GetBlockHash().ToString().c_str(), pindex->nHeight, pindexFrom->GetBlockHash().ToString().c_str());
            else
                return false;
        }
        pindex = pindex->pnext;
        if (pindex->GeneratedStakeModifier())
        {
            nStakeModifierHeight = pindex->nHeight;
            nStakeModifierTime = pindex->nTime;
        }
    }
    
	// LogPrintf(">> at height = %d\n", pindex->nHeight);
    nStakeModifier = pindex->nStakeModifier;
    return true;
}

// DeepOnion kernel protocol
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.block.nTime + txPrev.offset + txPrev.nTime + txPrev.vout.n + nTime) < bnTarget * nCoinDayWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coin age one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                  future proof-of-stake at the time of the coin's confirmation
//   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
//                       generate transaction for future advantage
//   txPrev.offset: offset of txPrev inside block, to reduce the chance of 
//                  nodes generating coinstake at the same time
//   txPrev.nTime: reduce the chance of nodes generating coinstake at the same
//                 time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
bool CheckStakeKernelHash(unsigned int nBits, CBlockIndex* pBlockFrom, CValidationState& state, CTransactionRef txPrevRef, unsigned int nTxPrevOffset,  
		const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, uint256& targetProofOfStake)
{
    if (nTimeTx < txPrevRef->nTime)  // Transaction timestamp violation
        return state.DoS(100, error("CheckStakeKernelHash() : nTime violation"));

    unsigned int nTimeBlockFrom = pBlockFrom->nTime;
    if (nTimeBlockFrom + nStakeMinAge > nTimeTx) // Min age requirement
    	return state.DoS(100, error("CheckStakeKernelHash() : min age violation"));

    arith_uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);

    uint256 hashBlockFrom = pBlockFrom->GetBlockHash();
    int64_t nValuePrev = txPrevRef->vout[prevout.n].nValue;

    arith_uint256 bnCoinDayWeight = arith_uint256(nValuePrev) * GetWeight((int64_t)txPrevRef->nTime, (int64_t)nTimeTx) / COIN / (24 * 60 * 60);
    targetProofOfStake = ArithToUint256(bnCoinDayWeight * bnTargetPerCoinDay);

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    uint64_t nStakeModifier = 0;
    int nStakeModifierHeight = 0;
    int64_t nStakeModifierTime = 0;

    // LogPrintf(">> pBlockFrom height = %d\n", pBlockFrom->nHeight);
    // LogPrintf(">> hashBlockFrom = %s\n", pBlockFrom->GetBlockHash().ToString().c_str());
    if (!GetKernelStakeModifier(pBlockFrom, nStakeModifier, nStakeModifierHeight, nStakeModifierTime))
        return false;
    
    ss << nStakeModifier;
    ss << nTimeBlockFrom << nTxPrevOffset << txPrevRef->nTime << prevout.n << nTimeTx;
    hashProofOfStake = Hash(ss.begin(), ss.end());
    
    /*
    LogPrintf("CheckStakeKernelHash() : using modifier 0x%016x at height=%d timestamp=%s for block from height=%d timestamp=%s\n",
            nStakeModifier, nStakeModifierHeight,
            DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nStakeModifierTime).c_str(),
			pBlockFrom->nHeight,
            DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pBlockFrom->GetBlockTime()).c_str());
   	LogPrintf("CheckStakeKernelHash() : check modifier=0x%016x nTimeBlockFrom=%u nTxPrevOffset=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
            nStakeModifier,
            nTimeBlockFrom, nTxPrevOffset, txPrevRef->nTime, prevout.n, nTimeTx,
            hashProofOfStake.ToString().c_str());
    */
    
    // Now check if proof-of-stake hash meets target protocol
    if (UintToArith256(hashProofOfStake) > bnCoinDayWeight * bnTargetPerCoinDay)
        return false;

    return true;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CBlockTreeDB& blockTreeDB, CBlockIndex* pindexPrev, CValidationState& state, const CBlock& block, uint256& hashProofOfStake, 
		uint256& targetProofOfStake, BlockMap& mapBlockIndex, CCoinsViewCache& view)
{
	LogPrintf(">> CheckProofOfStake\n");
	const CTransaction& tx = *block.vtx[1];
	unsigned int nBits = block.nBits;

	if (!tx.IsCoinStake())
        return state.DoS(100, error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString().c_str()));

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = tx.vin[0];

    // First try finding the previous transaction in database
	CDiskTxPos txindex;
	CTransactionRef txPrevRef;
    CBlockIndex* pBlockFrom = nullptr;
	unsigned int nTxPrevOffset = 0;
	if (blockTreeDB.ReadTxIndex(txin.prevout.hash, txindex)) 
	{
		CBlockHeader headerPrev;
		nTxPrevOffset = txindex.nTxOffset + 80;	// nTxOffset counts after header
        CAutoFile file(OpenBlockFile(txindex, true), SER_DISK, CLIENT_VERSION);
        if (file.IsNull())
        {
        	return state.DoS(100, error("CheckProofOfStake() : OpenBlockFile failed"));
        }
        try {
            file >> headerPrev;
            fseek(file.Get(), txindex.nTxOffset, SEEK_CUR);
            CMutableTransaction txPrev0;
            txPrev0.Unserialize(file);            
            txPrevRef = MakeTransactionRef(std::move(txPrev0));
            
            uint256 hashPrev = headerPrev.GetHash();
            BlockMap::iterator mi = mapBlockIndex.find(hashPrev);
            if (mi != mapBlockIndex.end()) {
            	pBlockFrom = mi->second;
            }
        } catch (const std::exception& e) {
        	return state.DoS(100, error("CheckProofOfStake() : Deserialize or I/O error, can't get prev tx - %s", e.what()));
        }
	}
	else
		return state.DoS(100, error(">> CheckProofOfStake() : blockTreeDB.ReadTxIndex failed.\n"));
	
    Coin coinPrev;
    if(!view.GetCoin(txin.prevout, coinPrev)){
        return state.DoS(100, error("CheckProofOfStake() : Stake prevout does not exist %s", txin.prevout.hash.ToString()));
    }

    if(pindexPrev->nHeight + 1 - coinPrev.nHeight < COINBASE_MATURITY){
        return state.DoS(100, error("CheckProofOfStake() : Stake prevout is not mature, expecting %i and only matured to %i", COINBASE_MATURITY, pindexPrev->nHeight + 1 - coinPrev.nHeight));
    }
    CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
    if(!blockFrom) {
        return state.DoS(100, error("CheckProofOfStake() : Block at height %i for prevout can not be loaded", coinPrev.nHeight));
    }
   
    // Verify signature
    /*
    if (!VerifySignature(coinPrev, txin.prevout.hash, tx, 0, SCRIPT_VERIFY_NONE))
        return state.DoS(100, error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString()));
    */
       
    if (!CheckStakeKernelHash(nBits, pBlockFrom, state, txPrevRef, nTxPrevOffset, txin.prevout, tx.nTime, hashProofOfStake, targetProofOfStake))
        return state.DoS(100, error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s", tx.GetHash().ToString().c_str(), hashProofOfStake.ToString().c_str())); 

    return true;
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx)
{
    // v0.3 protocol
    return (nTimeBlock == nTimeTx);
}

// Get stake modifier checksum
unsigned int GetStakeModifierChecksum(const CBlockIndex* pindex)
{
    // Hash previous checksum with flags, hashProofOfStake and nStakeModifier
    CDataStream ss(SER_GETHASH, 0);
    if (pindex->pprev != nullptr)
        ss << pindex->pprev->nStakeModifierChecksum;
    ss << pindex->nFlags << pindex->hashProofOfStake << pindex->nStakeModifier;
    uint256 hashChecksum = Hash(ss.begin(), ss.end());
    arith_uint256 hashChecksum0 = UintToArith256(hashChecksum);
    hashChecksum0 >>= (256 - 32);
    return hashChecksum0.Get64();
}

// Check stake modifier hard checkpoints
bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum)
{
	// LogPrintf(">> Height = %d, nStakeModifierChecksum = %x\n", nHeight, nStakeModifierChecksum);
    MapModifierCheckpoints& checkpoints = mapStakeModifierCheckpoints;

    if (checkpoints.count(nHeight))
        return nStakeModifierChecksum == checkpoints[nHeight];
    
    return true;
}

