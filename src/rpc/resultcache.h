//
// Created by pvl on 4/6/22.
//

#ifndef AIN_RESULTCACHE_H
#define AIN_RESULTCACHE_H

#include <atomic>
#include <string>
#include <map>
#include <set>
#include <optional>
#include <memory>
#include <univalue.h>
#include <rpc/request.h>

class RPCResultCache {
public:
    enum RPCCacheMode {
        None,
        Smart,
        All
    };

    void Init(RPCCacheMode mode);
    std::optional<UniValue> TryGet(const JSONRPCRequest &request);
    const UniValue& Set(const JSONRPCRequest &request, const UniValue &value);

private:
    std::atomic_bool syncFlag{false};
    std::set<std::string> smartModeList{};
    RPCCacheMode mode{RPCCacheMode::None};
    std::map<std::string, UniValue> cacheMap{};
    int cacheHeight{0};

    bool MayBeInvalidateCaches();
};

RPCResultCache& GetRPCResultCache();

int GetLastValidatedHeight();
void SetLastValidatedHeight(int height);

#endif //AIN_RESULTCACHE_H
