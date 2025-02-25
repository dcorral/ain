// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2018 The Bitcoin Core developers
// Copyright (c) 2017 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or http://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_PUBKEY_H
#define DEFI_PUBKEY_H

#include <hash.h>
#include <serialize.h>
#include <script/standard.h>
#include <uint256.h>

#include <stdexcept>
#include <vector>

const unsigned int BIP32_EXTKEY_SIZE = 74;

enum class KeyAddressType : uint8_t {
    DEFAULT, // Uses whatever is set in the pub / priv key
    COMPRESSED,
    UNCOMPRESSED
};

/** A reference to a CKey: the Hash160 of its serialized public key */
class CKeyID : public uint160
{
public:
    CKeyID() : uint160() {}
    explicit CKeyID(const uint160& in) : uint160(in) {}
    CKeyID(const uint160& in, const KeyAddressType type) : uint160(in), type(type) {}

    KeyAddressType type{KeyAddressType::DEFAULT};

    static std::optional<CKeyID> TryFromDestination(const CTxDestination &dest, KeyType filter=KeyType::UnknownKeyType) {
        // Explore switching TxDestType to a flag type. Then, we can easily take an allowed
        // flags here and use bit flag logic to decode only specific destinations
        switch (dest.index()) {
            case PKHashType:
                if ((filter & KeyType::PKHashKeyType) == KeyType::PKHashKeyType) {
                    return CKeyID(std::get<PKHash>(dest));
                }
                break;
            case WitV0KeyHashType:
                if ((filter & KeyType::WPKHashKeyType) == KeyType::WPKHashKeyType) {
                    return CKeyID(std::get<WitnessV0KeyHash>(dest));
                }
                break;
            case ScriptHashType:
                if ((filter & KeyType::ScriptHashKeyType) == KeyType::ScriptHashKeyType) {
                    return CKeyID(std::get<ScriptHash>(dest));
                }
                break;
            case WitV16KeyEthHashType:
                if ((filter & KeyType::EthHashKey) == KeyType::EthHashKey) {
                    return CKeyID(std::get<WitnessV16EthHash>(dest));
                }
                break;
            default: 
                return {};
        }
        return {};
    }

    static CKeyID FromOrDefaultDestination(const CTxDestination &dest, KeyType filter=KeyType::UnknownKeyType) {
        auto key = TryFromDestination(dest, filter);
        if (key) {
            return *key;
        }
        return {};
    }
};

typedef uint256 ChainCode;

/** An encapsulated public key. */
class CPubKey
{
public:
    /**
     * secp256k1:
     */
    static constexpr unsigned int PUBLIC_KEY_SIZE             = 65;
    static constexpr unsigned int COMPRESSED_PUBLIC_KEY_SIZE  = 33;
    static constexpr unsigned int SIGNATURE_SIZE              = 72;
    static constexpr unsigned int COMPACT_SIGNATURE_SIZE      = 65;
    /**
     * see www.keylength.com
     * script supports up to 75 for single byte push
     */
    static_assert(
        PUBLIC_KEY_SIZE >= COMPRESSED_PUBLIC_KEY_SIZE,
        "COMPRESSED_PUBLIC_KEY_SIZE is larger than PUBLIC_KEY_SIZE");

private:

    /**
     * Just store the serialized data.
     * Its length can very cheaply be computed from the first byte.
     */
    unsigned char vch[PUBLIC_KEY_SIZE];

    //! Compute the length of a pubkey with a given first byte.
    unsigned int static GetLen(unsigned char chHeader)
    {
        if (chHeader == 2 || chHeader == 3)
            return COMPRESSED_PUBLIC_KEY_SIZE;
        if (chHeader == 4 || chHeader == 6 || chHeader == 7)
            return PUBLIC_KEY_SIZE;
        return 0;
    }

    //! Set this key data to be invalid
    void Invalidate()
    {
        vch[0] = 0xFF;
    }

public:

    bool static ValidSize(const std::vector<unsigned char> &vch) {
      return vch.size() > 0 && GetLen(vch[0]) == vch.size();
    }

    //! Construct an invalid public key.
    CPubKey()
    {
        Invalidate();
    }

    //! Initialize a public key using begin/end iterators to byte data.
    template <typename T>
    void Set(const T pbegin, const T pend)
    {
        int len = pend == pbegin ? 0 : GetLen(pbegin[0]);
        if (len && len == (pend - pbegin))
            memcpy(vch, (unsigned char*)&pbegin[0], len);
        else
            Invalidate();
    }

    //! Construct a public key using begin/end iterators to byte data.
    template <typename T>
    CPubKey(const T pbegin, const T pend)
    {
        Set(pbegin, pend);
    }

    //! Construct a public key from a byte vector.
    explicit CPubKey(const std::vector<unsigned char>& _vch)
    {
        Set(_vch.begin(), _vch.end());
    }

    //! Simple read-only vector-like interface to the pubkey data.
    unsigned int size() const { return GetLen(vch[0]); }
    const unsigned char* data() const { return vch; }
    const unsigned char* begin() const { return vch; }
    const unsigned char* end() const { return vch + size(); }
    const unsigned char& operator[](unsigned int pos) const { return vch[pos]; }

    //! Comparator implementation.
    friend bool operator==(const CPubKey& a, const CPubKey& b)
    {
        return a.vch[0] == b.vch[0] &&
               memcmp(a.vch, b.vch, a.size()) == 0;
    }
    friend bool operator!=(const CPubKey& a, const CPubKey& b)
    {
        return !(a == b);
    }
    friend bool operator<(const CPubKey& a, const CPubKey& b)
    {
        return a.vch[0] < b.vch[0] ||
               (a.vch[0] == b.vch[0] && memcmp(a.vch, b.vch, a.size()) < 0);
    }

    //! Implement serialization, as if this was a byte vector.
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        unsigned int len = size();
        ::WriteCompactSize(s, len);
        s.write((char*)vch, len);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        unsigned int len = ::ReadCompactSize(s);
        if (len <= PUBLIC_KEY_SIZE) {
            s.read((char*)vch, len);
        } else {
            // invalid pubkey, skip available data
            char dummy;
            while (len--)
                s.read(&dummy, 1);
            Invalidate();
        }
    }

    //! Get the KeyID of this public key (hash of its serialization)
    CKeyID GetID() const
    {
        return CKeyID(Hash160(vch, vch + size()));
    }

    CKeyID GetEthID() const
    {
        const size_t HEADER_OFFSET{1};
        return CKeyID(Sha3({vch + HEADER_OFFSET, vch + size()}));
    }

    //! Get the 256-bit hash of this public key.
    uint256 GetHash() const
    {
        return Hash(vch, vch + size());
    }

    /*
     * Check syntactic correctness.
     *
     * Note that this is consensus critical as CheckSig() calls it!
     */
    bool IsValid() const
    {
        return size() > 0;
    }

    //! fully validate whether this is a valid public key (more expensive than IsValid())
    bool IsFullyValid() const;

    //! Check whether this is a compressed public key.
    bool IsCompressed() const
    {
        return size() == COMPRESSED_PUBLIC_KEY_SIZE;
    }

    /**
     * Verify a DER signature (~72 bytes).
     * If this public key is not fully valid, the return value will be false.
     */
    bool Verify(const uint256& hash, const std::vector<unsigned char>& vchSig) const;

    /**
     * Check whether a signature is normalized (lower-S).
     */
    static bool CheckLowS(const std::vector<unsigned char>& vchSig);

    //! Recover a public key from a compact signature.
    bool RecoverCompact(const uint256& hash, const std::vector<unsigned char>& vchSig);

    //! Turn this public key into an uncompressed public key.
    bool Decompress();

    //! Turn this public key into a compressed public key.
    bool Compress();

    //! Derive BIP32 child pubkey.
    bool Derive(CPubKey& pubkeyChild, ChainCode &ccChild, unsigned int nChild, const ChainCode& cc) const;

    static bool TryRecoverSigCompat(const std::vector<unsigned char>& vchSig, std::vector<unsigned char>* sig = nullptr);
};

inline std::pair<CPubKey, CPubKey> GetBothPubkeyCompressions(const CPubKey &key) {
    auto keyCopy = key;
    if (key.IsCompressed()) {
        keyCopy.Decompress();
        return {keyCopy, key};
    } else {
        keyCopy.Compress();
    }
    return {key, keyCopy};
}

struct CExtPubKey {
    unsigned char nDepth;
    unsigned char vchFingerprint[4];
    unsigned int nChild;
    ChainCode chaincode;
    CPubKey pubkey;

    friend bool operator==(const CExtPubKey &a, const CExtPubKey &b)
    {
        return a.nDepth == b.nDepth &&
            memcmp(&a.vchFingerprint[0], &b.vchFingerprint[0], sizeof(vchFingerprint)) == 0 &&
            a.nChild == b.nChild &&
            a.chaincode == b.chaincode &&
            a.pubkey == b.pubkey;
    }

    void Encode(unsigned char code[BIP32_EXTKEY_SIZE]) const;
    void Decode(const unsigned char code[BIP32_EXTKEY_SIZE]);
    bool Derive(CExtPubKey& out, unsigned int nChild) const;

    void Serialize(CSizeComputer& s) const
    {
        // Optimized implementation for ::GetSerializeSize that avoids copying.
        s.seek(BIP32_EXTKEY_SIZE + 1); // add one byte for the size (compact int)
    }
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        unsigned int len = BIP32_EXTKEY_SIZE;
        ::WriteCompactSize(s, len);
        unsigned char code[BIP32_EXTKEY_SIZE];
        Encode(code);
        s.write((const char *)&code[0], len);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        unsigned int len = ::ReadCompactSize(s);
        unsigned char code[BIP32_EXTKEY_SIZE];
        if (len != BIP32_EXTKEY_SIZE)
            throw std::runtime_error("Invalid extended key size\n");
        s.read((char *)&code[0], len);
        Decode(code);
    }
};

/** Users of this module must hold an ECCVerifyHandle. The constructor and
 *  destructor of these are not allowed to run in parallel, though. */
class ECCVerifyHandle
{
    static int refcount;

public:
    ECCVerifyHandle();
    ~ECCVerifyHandle();
};

#endif // DEFI_PUBKEY_H
