#pragma once
// XPRESS for the game's datagrams, on their plaintext (NetFeature::Compression).
//
// b63c84d packed packets with XPRESS in the EOS_P2P_SendPacket wrapper and was reverted (a43fb98): the game encrypts
// every datagram (AES-CTR, 7B8060) before it reaches EOS, so that wrapper only ever saw ciphertext, which does not
// compress - nothing was ever packed, and the "start message that fits packed" it promised never fitted. (The same
// blindness made packetfit look for stubs in ciphertext until e1649ac.)
//
// Here the plaintext is packed where the game encrypts it and unpacked where the game decrypts it:
//   12CEBAA  the flush (12CEA10) calls 7B8060(cipher, records, size) to encrypt the records after the 8-byte header.
//            NetEncryptHook packs the records first when that saves enough (kPackMinimum, kPackSavingMin); the CRC
//            in the header was taken over the unpacked records already (12CEA79).
//   12CE16B  the receive (12CDF20) calls D2360(cipher, ciphertext, size) to decrypt into the cipher's std::string
//            (+0, the game's allocator); then it checks the header's CRC over that string and parses its records.
//            NetDecryptHook unpacks a packed plaintext in that string before the game looks at it, so the CRC, the
//            duplicate check and the parser see the records as they were sent.
// A packed plaintext starts with kPackMagic ('X' 'P' 'R' 1, then the unpacked size as u16): a record header's low
// byte is always 0 (types are (sub | id << 4) << 8), so no plaintext the game writes starts like that.
//
// A member without this would drop every packed datagram (its CRC does not match what it decrypts), so datagrams are
// packed only while the whole room runs it (NetFeatureActive). Unpacking needs no gate: a packed plaintext is
// recognised by its magic.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "patches.h"

namespace multislot {

constexpr std::size_t kPackHeader = 6;
constexpr std::uint8_t kPackMagic[4] = {'X', 'P', 'R', 1};
constexpr std::size_t kPackMinimum = 128;    // smaller records are not worth the work
constexpr std::size_t kPackSavingMin = 16;   // packing must save at least this many bytes
constexpr std::size_t kMaxUnpacked = 0x1000; // the game never sends more than 1408 (12D0BE6) nor receives more (12C8D1D)

bool PackingAvailable();
// `data` packed (with the header) into `out`; false when it is too small, packing saves too little, or XPRESS is
// not there.
bool PackPlaintext(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out);
bool IsPackedPlaintext(const std::uint8_t* data, std::size_t size);
// A packed plaintext's original bytes; false when it is not one or it is damaged.
bool UnpackPlaintext(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out);

std::vector<CallSite> NetCompressCalls();
void* NetCompressCallHandler(std::uint32_t rva);
// `gameBase`: EDF.dll; `enabled`: [Netcode] Compression (packing still waits for the whole room).
void InitNetCompress(const unsigned char* gameBase, bool enabled);
// Datagrams packed and bytes saved since the last call (the log).
void TakePackStats(std::uint64_t& packed, std::uint64_t& saved, std::uint64_t& unpacked);

}  // namespace multislot
