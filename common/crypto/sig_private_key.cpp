/* Copyright 2018 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "sig_private_key.h"
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <algorithm>
#include <memory>
#include <vector>
#include "base64.h"  //simple base64 enc/dec routines
#include "crypto_shared.h"
#include "error.h"
#include "hex_string.h"
#include "sig.h"
#include "sig_public_key.h"
/***Conditional compile untrusted/trusted***/
#if _UNTRUSTED_
#include <openssl/crypto.h>
#include <stdio.h>
#else
#include "tSgxSSL_api.h"
#endif
/***END Conditional compile untrusted/trusted***/

namespace pcrypto = pdo::crypto;
namespace constants = pdo::crypto::constants;

// Typedefs for memory management
// Specify type and destroy function type for unique_ptrs
typedef std::unique_ptr<BIO, void (*)(BIO*)> BIO_ptr;
typedef std::unique_ptr<EVP_CIPHER_CTX, void (*)(EVP_CIPHER_CTX*)> CTX_ptr;
typedef std::unique_ptr<BN_CTX, void (*)(BN_CTX*)> BN_CTX_ptr;
typedef std::unique_ptr<BIGNUM, void (*)(BIGNUM*)> BIGNUM_ptr;
typedef std::unique_ptr<EC_GROUP, void (*)(EC_GROUP*)> EC_GROUP_ptr;
typedef std::unique_ptr<EC_POINT, void (*)(EC_POINT*)> EC_POINT_ptr;
typedef std::unique_ptr<EC_KEY, void (*)(EC_KEY*)> EC_KEY_ptr;
typedef std::unique_ptr<ECDSA_SIG, void (*)(ECDSA_SIG*)> ECDSA_SIG_ptr;
// Error handling
namespace Error = pdo::error;

// Compute SHA256 digest
static void SHA256Hash(
    const unsigned char* buf, int buf_size, unsigned char hash[SHA256_DIGEST_LENGTH])
{
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, buf, buf_size);
    SHA256_Final(hash, &sha256);
}  // pcrypto::SHA256Hash

// XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
// Utility function: Deserialize ECDSA Private Key
// throws RuntimeError, ValueError
EC_KEY* deserializeECDSAPrivateKey(const std::string& encoded)
{
    BIO_ptr bio(BIO_new_mem_buf(encoded.c_str(), -1), BIO_free_all);
    if (!bio)
    {
        std::string msg("Crypto Error (deserializeECDSAPrivateKey): Could not create BIO");
        throw Error::RuntimeError(msg);
    }

    EC_KEY* private_key = PEM_read_bio_ECPrivateKey(bio.get(), NULL, NULL, NULL);
    if (!private_key)
    {
        std::string msg(
            "Crypto Error (deserializeECDSAPrivateKey): Could not "
            "deserialize private ECDSA key");
        throw Error::ValueError(msg);
    }
    return private_key;
}  // deserializeECDSAPrivateKey

// XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
// Constructor
// throws RuntimeError
pcrypto::sig::PrivateKey::PrivateKey()
{
    EC_KEY_ptr private_key(EC_KEY_new(), EC_KEY_free);

    if (!private_key)
    {
        std::string msg("Crypto Error (sig::PrivateKey()): Could not create new EC_KEY");
        throw Error::RuntimeError(msg);
    }

    EC_GROUP_ptr ec_group(EC_GROUP_new_by_curve_name(constants::CURVE), EC_GROUP_clear_free);
    if (!ec_group)
    {
        std::string msg("Crypto Error (sig::PrivateKey()): Could not create EC_GROUP");
        throw Error::RuntimeError(msg);
    }

    if (!EC_KEY_set_group(private_key.get(), ec_group.get()))
    {
        std::string msg("Crypto Error (sig::PrivateKey()): Could not set EC_GROUP");
        throw Error::RuntimeError(msg);
    }

    if (!EC_KEY_generate_key(private_key.get()))
    {
        std::string msg("Crypto Error (sig::PrivateKey()): Could not generate EC_KEY");
        throw Error::RuntimeError(msg);
    }

    private_key_ = EC_KEY_dup(private_key.get());
    if (!private_key_)
    {
        std::string msg("Crypto Error (sig::PrivateKey()): Could not dup private EC_KEY");
        throw Error::RuntimeError(msg);
    }
}  // pcrypto::sig::PrivateKey::PrivateKey

// Constructor from encoded string
// throws RuntimeError, ValueError
pcrypto::sig::PrivateKey::PrivateKey(const std::string& encoded)
{
    private_key_ = deserializeECDSAPrivateKey(encoded);
}  // pcrypto::sig::PrivateKey::PrivateKey

// XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
// Copy constructor
// throws RuntimeError
pcrypto::sig::PrivateKey::PrivateKey(const pcrypto::sig::PrivateKey& privateKey)
{
    private_key_ = EC_KEY_dup(privateKey.private_key_);
    if (!private_key_)
    {
        std::string msg("Crypto Error (sig::PrivateKey() copy): Could not copy private key");
        throw Error::RuntimeError(msg);
    }
}  // pcrypto::sig::PrivateKey::PrivateKey (copy constructor)

// XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
// Move constructor
// throws RuntimeError
pcrypto::sig::PrivateKey::PrivateKey(pcrypto::sig::PrivateKey&& privateKey)
{
    private_key_ = privateKey.private_key_;
    privateKey.private_key_ = nullptr;
    if (!private_key_)
    {
        std::string msg("Crypto Error (sig::PrivateKey() move): Cannot move null private key");
        throw Error::RuntimeError(msg);
    }
}  // pcrypto::sig::PrivateKey::PrivateKey (move constructor)

// XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
// Destructor
pcrypto::sig::PrivateKey::~PrivateKey()
{
    if (private_key_)
        EC_KEY_free(private_key_);
}  // pcrypto::sig::PrivateKey::~PrivateKey

// XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
// assignment operator overload
// throws RuntimeError
pcrypto::sig::PrivateKey& pcrypto::sig::PrivateKey::operator=(
    const pcrypto::sig::PrivateKey& privateKey)
{
    if (this == &privateKey)
        return *this;
    if (private_key_)
        EC_KEY_free(private_key_);
    private_key_ = EC_KEY_dup(privateKey.private_key_);
    if (!private_key_)
    {
        std::string msg("Crypto Error (sig::PrivateKey operator =): Could not copy private key");
        throw Error::RuntimeError(msg);
    }
    return *this;
}  // pcrypto::sig::PrivateKey::operator =

// XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
// Deserialize ECDSA Private Key
// thorws RuntimeError, ValueError
void pcrypto::sig::PrivateKey::Deserialize(const std::string& encoded)
{
    EC_KEY* key = deserializeECDSAPrivateKey(encoded);
    if (private_key_)
        EC_KEY_free(private_key_);
    private_key_ = key;
}  // pcrypto::sig::PrivateKey::Deserialize

// XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
// Derive Digital Signature public key from private key
// throws RuntimeError
pcrypto::sig::PublicKey pcrypto::sig::PrivateKey::GetPublicKey() const
{
    PublicKey publicKey(*this);
    return publicKey;
}  // pcrypto::sig::GetPublicKey()

// XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
// Serialize ECDSA PrivateKey
// thorws RuntimeError
std::string pcrypto::sig::PrivateKey::Serialize() const
{
    BIO_ptr bio(BIO_new(BIO_s_mem()), BIO_free_all);

    if (!bio)
    {
        std::string msg("Crypto Error (Serialize): Could not create BIO");
        throw Error::RuntimeError(msg);
    }

    PEM_write_bio_ECPrivateKey(bio.get(), private_key_, NULL, NULL, 0, 0, NULL);

    int keylen = BIO_pending(bio.get());

    ByteArray pem_str(keylen + 1);
    if (!BIO_read(bio.get(), pem_str.data(), keylen))
    {
        std::string msg("Crypto Error (Serialize): Could not read BIO");
        throw Error::RuntimeError(msg);
    }
    pem_str[keylen] = '\0';
    std::string str((char*)(pem_str.data()));

    return str;
}  // pcrypto::sig::PrivateKey::Serialize

// XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX
// Computes SHA256 hash of message.data(), signs with ECDSA privkey and
// returns ByteArray containing raw binary data
// throws RuntimeError
ByteArray pcrypto::sig::PrivateKey::SignMessage(const ByteArray& message) const
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    // Hash
    SHA256Hash((const unsigned char*)message.data(), message.size(), hash);
    // Then Sign

    ECDSA_SIG_ptr sig(ECDSA_do_sign((const unsigned char*)hash, SHA256_DIGEST_LENGTH, private_key_),
        ECDSA_SIG_free);
    if (!sig)
    {
        std::string msg("Crypto Error (SignMessage): Could not compute ECDSA signature");
        throw Error::RuntimeError(msg);
    }
    const BIGNUM* sc;
    const BIGNUM* rc;
    BIGNUM* r = nullptr;
    BIGNUM* s = nullptr;

    ECDSA_SIG_get0(sig.get(), &rc, &sc);

    s = BN_dup(sc);
    if (!s)
    {
        std::string msg("Crypto Error (SignMessage): Could not dup BIGNUM for s");
        throw Error::RuntimeError(msg);
    }
    r = BN_dup(rc);
    if (!r)
    {
        std::string msg("Crypto Error (SignMessage): Could not dup BIGNUM for r");
        throw Error::RuntimeError(msg);
    }
    BIGNUM_ptr ord(BN_new(), BN_free);
    if (!ord)
    {
        std::string msg("Crypto Error (SignMessage): Could not create BIGNUM for ord");
        throw Error::RuntimeError(msg);
    }

    BIGNUM_ptr ordh(BN_new(), BN_free);
    if (!ordh)
    {
        std::string msg("Crypto Error (SignMessage): Could not create BIGNUM for ordh");
        throw Error::RuntimeError(msg);
    }

    int res = EC_GROUP_get_order(EC_KEY_get0_group(private_key_), ord.get(), NULL);
    if (!res)
    {
        std::string msg("Crypto Error (SignMessage): Could not get order");
        throw Error::RuntimeError(msg);
    }

    res = BN_rshift(ordh.get(), ord.get(), 1);

    if (!res)
    {
        std::string msg("Crypto Error (SignMessage): Could not shft order BN");
        throw Error::RuntimeError(msg);
    }

    if (BN_cmp(s, ordh.get()) >= 0)
    {
        res = BN_sub(s, ord.get(), s);
        if (!res)
        {
            std::string msg("Crypto Error (SignMessage): Could not sub BNs");
            throw Error::RuntimeError(msg);
        }
    }

    res = ECDSA_SIG_set0(sig.get(), r, s);
    if (!res)
    {
        std::string msg("Crypto Error (SignMessage): Could not set r and s");
        throw Error::RuntimeError(msg);
    }

    // The -1 here is because we canonoicalize the signature as in Bitcoin
    unsigned int der_sig_size = i2d_ECDSA_SIG(sig.get(), nullptr);
    ByteArray der_SIG(der_sig_size, 0);
    unsigned char* data = der_SIG.data();
    res = i2d_ECDSA_SIG(sig.get(), &data);

    if (!res)
    {
        std::string msg("Crypto Error (SignMessage): Could not convert signatureto DER");
        throw Error::RuntimeError(msg);
    }
    return der_SIG;
}  // pcrypto::sig::PrivateKey::SignMessage
