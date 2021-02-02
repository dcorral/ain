#ifndef DEFI_POS_KERNEL_H
#define DEFI_POS_KERNEL_H

#include <uint256.h>
#include <arith_uint256.h>
#include <consensus/params.h>
#include <streams.h>
#include <amount.h>

class CWallet;

class COutPoint;

class CBlock;

class CTransaction;

class CCoinsViewCache;

class CKeyID;

class CCustomCSView;

namespace pos {

    struct CheckKernelHashRes {
        bool hashOk;
        arith_uint256 hashProofOfStake;
    };

/// Calculate PoS kernel hash
    uint256
    CalcKernelHash(const uint256& stakeModifier, int64_t height, int64_t coinstakeTime, const uint256& masternodeID, const Consensus::Params& params);

/// Check whether stake kernel meets hash target
/// Sets hashProofOfStake, hashOk is true of the kernel meets hash target
    CheckKernelHashRes
    CheckKernelHash(const uint256& stakeModifier, uint32_t nBits, int64_t height, int64_t coinstakeTime, const uint256& masternodeID, const Consensus::Params& params);

/// Stake Modifier (hash modifier of proof-of-stake)
    uint256 ComputeStakeModifier(const uint256& prevStakeModifier, const CKeyID& key);
}

#endif // DEFI_POS_KERNEL_H
