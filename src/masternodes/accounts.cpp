// Copyright (c) DeFi Blockchain Developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#include <masternodes/accounts.h>

static CFuturesUserHeightPrefixKey TranslateKeyToHeightPrefix(CFuturesUserOwnerPrefixKey const & key)
{
    return {key.height, key.owner, key.txn};
}

static CFuturesUserOwnerPrefixKey TranslateKeyToOwnerPrefix(CFuturesUserHeightPrefixKey const & key)
{
    return {key.owner, key.height, key.txn};
}

void CAccountsView::ForEachBalance(std::function<bool(CScript const &, CTokenAmount const &)> callback, BalanceKey const & start)
{
    ForEach<ByBalanceKey, BalanceKey, CAmount>([&callback] (BalanceKey const & key, CAmount val) {
        return callback(key.owner, CTokenAmount{key.tokenID, val});
    }, start);
}

CTokenAmount CAccountsView::GetBalance(CScript const & owner, DCT_ID tokenID) const
{
    CAmount val;
    bool ok = ReadBy<ByBalanceKey>(BalanceKey{owner, tokenID}, val);
    if (ok) {
        return CTokenAmount{tokenID, val};
    }
    return CTokenAmount{tokenID, 0};
}

Res CAccountsView::SetBalance(CScript const & owner, CTokenAmount amount)
{
    if (amount.nValue != 0) {
        WriteBy<ByBalanceKey>(BalanceKey{owner, amount.nTokenId}, amount.nValue);
    } else {
        EraseBy<ByBalanceKey>(BalanceKey{owner, amount.nTokenId});
    }
    return Res::Ok();
}

Res CAccountsView::AddBalance(CScript const & owner, CTokenAmount amount)
{
    if (amount.nValue == 0) {
        return Res::Ok();
    }
    auto balance = GetBalance(owner, amount.nTokenId);
    auto res = balance.Add(amount.nValue);
    if (!res.ok) {
        return res;
    }
    return SetBalance(owner, balance);
}

Res CAccountsView::SubBalance(CScript const & owner, CTokenAmount amount)
{
    if (amount.nValue == 0) {
        return Res::Ok();
    }
    auto balance = GetBalance(owner, amount.nTokenId);
    auto res = balance.Sub(amount.nValue);
    if (!res.ok) {
        return res;
    }
    return SetBalance(owner, balance);
}

Res CAccountsView::AddBalances(CScript const & owner, CBalances const & balances)
{
    for (const auto& kv : balances.balances) {
        auto res = AddBalance(owner, CTokenAmount{kv.first, kv.second});
        if (!res.ok) {
            return res;
        }
    }
    return Res::Ok();
}

Res CAccountsView::SubBalances(CScript const & owner, CBalances const & balances)
{
    for (const auto& kv : balances.balances) {
        auto res = SubBalance(owner, CTokenAmount{kv.first, kv.second});
        if (!res.ok) {
            return res;
        }
    }
    return Res::Ok();
}

void CAccountsView::ForEachAccount(std::function<bool(CScript const &)> callback, CScript const & start)
{
    ForEach<ByHeightKey, CScript, uint32_t>([&callback] (CScript const & owner, CLazySerialize<uint32_t>) {
        return callback(owner);
    }, start);
}

Res CAccountsView::UpdateBalancesHeight(CScript const & owner, uint32_t height)
{
    WriteBy<ByHeightKey>(owner, height);
    return Res::Ok();
}

uint32_t CAccountsView::GetBalancesHeight(CScript const & owner)
{
    uint32_t height;
    bool ok = ReadBy<ByHeightKey>(owner, height);
    return ok ? height : 0;
}

void CAccountsView::CreateFuturesMultiIndexIfNeeded()
{
    CFuturesUserOwnerPrefixKey anyOwnerKey{{}, ~0u, ~0u};
    if (auto it = LowerBound<ByFuturesSwapKeyOwner>(anyOwnerKey); it.Valid()) {
        return;
    }

    LogPrint(BCLog::BENCH, "FuturesSwap - Adding multi index in progress...\n");

    auto startTime = GetTimeMillis();

    CFuturesUserHeightPrefixKey startKey{~0u, {}, ~0u};
    auto it = LowerBound<ByFuturesSwapKey>(startKey);
    for (; it.Valid(); it.Next()) {
        WriteBy<ByFuturesSwapKeyOwner>(TranslateKeyToOwnerPrefix(it.Key()), '\0');
    }

    LogPrint(BCLog::BENCH, "FuturesSwap - Multi index took: %dms\n", GetTimeMillis() - startTime);
}

Res CAccountsView::StoreFuturesUserValues(const CFuturesUserHeightPrefixKey& key, const CFuturesUserValue& futures)
{
    if (!WriteBy<ByFuturesSwapKey>(key, futures)) {
        return Res::Err("Failed to store futures");
    }
    if (!WriteBy<ByFuturesSwapKeyOwner>(TranslateKeyToOwnerPrefix(key), '\0')) {
        return Res::Err("Failed to store futures by owner key");
    }

    return Res::Ok();
}

void CAccountsView::ForEachFuturesUserValues(std::function<bool(const CFuturesUserHeightPrefixKey&, const CFuturesUserValue&)> callback, const CFuturesUserHeightPrefixKey& start)
{
    if (start.owner.empty()) {
        ForEach<ByFuturesSwapKey, CFuturesUserHeightPrefixKey, CFuturesUserValue>(callback, start);
    } else {
        ForEach<ByFuturesSwapKeyOwner, CFuturesUserOwnerPrefixKey, char>([&](const CFuturesUserOwnerPrefixKey& ownerKey, const char&) {
            CFuturesUserHeightPrefixKey key = TranslateKeyToHeightPrefix(ownerKey);
            return callback(key, *GetFuturesUserValues(key));
        }, TranslateKeyToOwnerPrefix(start));
    }
}

Res CAccountsView::EraseFuturesUserValues(const CFuturesUserHeightPrefixKey& key)
{
    if (!EraseBy<ByFuturesSwapKey>(key)) {
        return Res::Err("Failed to erase futures");
    }
    if (!EraseBy<ByFuturesSwapKeyOwner>(TranslateKeyToOwnerPrefix(key))) {
        return Res::Err("Failed to erase futures by owner key");
    }

    return Res::Ok();
}

boost::optional<uint32_t> CAccountsView::GetMostRecentFuturesHeight()
{
    const CFuturesUserHeightPrefixKey key{std::numeric_limits<uint32_t>::max(), {}, std::numeric_limits<uint32_t>::max()};
    auto it = LowerBound<ByFuturesSwapKey>(key);
    if (it.Valid()) {
        return it.Key().height;
    }

    return {};
}

ResVal<CFuturesUserValue> CAccountsView::GetFuturesUserValues(const CFuturesUserHeightPrefixKey& key) {
    CFuturesUserValue source;
    if (!ReadBy<ByFuturesSwapKey>(key, source)) {
        return Res::Err("Failed to read futures source");
    }

    return {source, Res::Ok()};
}
