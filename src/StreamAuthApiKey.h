#pragma once
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace dvbstreamer5::stream_auth::api_key {
inline bool isLowerHex(const std::string& key,std::size_t size=64) {
    if(key.size()!=size)return false;
    return std::all_of(key.begin(),key.end(),[](unsigned char c){
        return (c>='0'&&c<='9')||(c>='a'&&c<='f');
    });
}
inline std::string hex(const unsigned char* data,std::size_t count) {
    static constexpr char chars[]="0123456789abcdef";
    std::string value;value.reserve(count*2U);
    for(std::size_t i=0;i<count;++i){
        value.push_back(chars[data[i]>>4U]);
        value.push_back(chars[data[i]&15U]);
    }
    return value;
}
inline std::string generate() {
    std::array<unsigned char,32> bytes{};
    if(RAND_bytes(bytes.data(),static_cast<int>(bytes.size()))!=1)
        return {};
    return hex(bytes.data(),bytes.size());
}
inline std::string sha256(const std::string& raw) {
    std::array<unsigned char,EVP_MAX_MD_SIZE> digest{};
    unsigned int size=0;
    if(EVP_Digest(raw.data(),raw.size(),digest.data(),&size,EVP_sha256(),nullptr)!=1 ||
       size!=32U)return {};
    return hex(digest.data(),size);
}
inline bool matches(const std::string& savedDigest,const std::string& candidate) {
    if(!isLowerHex(savedDigest)||!isLowerHex(candidate))return false;
    const auto supplied=sha256(candidate);
    return supplied.size()==savedDigest.size() &&
        CRYPTO_memcmp(savedDigest.data(),supplied.data(),savedDigest.size())==0;
}
inline std::string normalizedAddress(const std::string& text) {
    boost::system::error_code ec;
    const auto addr=boost::asio::ip::make_address(text,ec);
    if(ec)return {};
    if(addr.is_v6() && addr.to_v6().is_v4_mapped()){
        // Boost.Asio address_v6 has no to_v4() on Ubuntu 24.04.
        // Decode the final IPv4 octets of a valid v4-mapped IPv6 address.
        const auto bytes=addr.to_v6().to_bytes();
        boost::asio::ip::address_v4::bytes_type v4{{
            bytes[12],bytes[13],bytes[14],bytes[15]}};
        return boost::asio::ip::address_v4(v4).to_string();
    }
    return addr.to_string();
}
inline std::vector<std::string> addressList(const std::string& csv) {
    std::vector<std::string> addresses;
    if(csv.size()>1024U)return {};
    std::string part;
    const auto commit=[&]() {
        if(part.empty())return true;
        const auto normalized=normalizedAddress(part);
        part.clear();
        if(normalized.empty()||addresses.size()>=32U)return false;
        if(std::find(addresses.begin(),addresses.end(),normalized)==addresses.end())
            addresses.push_back(normalized);
        return true;
    };
    for(const unsigned char c:csv){
        if(c==','||c==';'||std::isspace(c)){
            if(!commit())return {};
        }else part.push_back(static_cast<char>(c));
    }
    if(!commit())return {};
    return addresses;
}
inline bool validAllowedIps(const std::string& csv) {
    const auto list=addressList(csv);
    if(list.empty())return false;
    // Do not accept partially parsed lists (e.g. one valid + one bad IP).
    std::size_t expected=0;bool inToken=false;
    for(const unsigned char c:csv){
        if(c==','||c==';'||std::isspace(c))inToken=false;
        else if(!inToken){++expected;inToken=true;}
    }
    return expected<=32 && list.size()<=expected &&
        // Duplicate addresses are permitted, but malformed addresses cannot
        // be silently discarded. Parse every individual token independently.
        [&](){
            std::string token;
            const auto valid=[&](){
                if(token.empty())return true;
                const bool okay=!normalizedAddress(token).empty();
                token.clear();return okay;
            };
            for(const unsigned char c:csv){
                if(c==','||c==';'||std::isspace(c)){
                    if(!valid())return false;
                }else token.push_back(static_cast<char>(c));
            }
            return valid();
        }();
}
inline bool isPeerAllowed(const std::string& csv,const std::string& peerIp) {
    const auto candidate=normalizedAddress(peerIp);
    if(candidate.empty()||!validAllowedIps(csv))return false;
    const auto list=addressList(csv);
    return std::find(list.begin(),list.end(),candidate)!=list.end();
}
inline bool authorize(const std::string& savedDigest,
                      const std::string& allowedIps,
                      const std::string& candidate,const std::string& peerIp){
    return matches(savedDigest,candidate) && isPeerAllowed(allowedIps,peerIp);
}
// Optional session-to-subscriber correlation is exposed only after both
// middleware authorization and the least-privilege sessions API are enabled.
inline bool backendIdVisible(bool middlewareEnabled,const std::string& keyDigest) {
    return middlewareEnabled && isLowerHex(keyDigest);
}
inline bool inScope(const std::string& path,const std::string& method) {
    return (path=="/api/stream-auth/sessions" && method=="GET") ||
           (path=="/api/stream-auth/sessions/disconnect" && method=="POST");
}
} // namespace dvbstreamer5::stream_auth::api_key
