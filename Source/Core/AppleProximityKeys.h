//
// AirPodsDesktop - AirPods Desktop User Experience Enhancement Program.
// Copyright (C) 2021-2022 SpriteOvO
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//

#pragma once

// Proximity keys of a pair of AirPods. Pure logic, no platform calls.
//
// AirPods advertise with Bluetooth resolvable private addresses (RPA) that rotate. Every device
// the AirPods are paired with through Apple's stack receives two 16-byte keys:
//
//   - the Identity Resolving Key (IRK): an RPA is `prand (24 bit) || hash (24 bit)` with
//     `hash = ah(IRK, prand)`, the address hash function of the Bluetooth Core specification
//     (Vol 3, Part H, 2.2.2). Knowing the IRK proves that an advertisement comes from *these*
//     AirPods, which removes every heuristic the state manager otherwise needs.
//   - the proximity encryption key: the last 16 bytes of the proximity pairing message are one
//     AES-128-ECB block. Decrypted, bytes 1, 2 and 3 carry the battery of the broadcasting pod, the
//     other pod and the case in 1 % steps (bits 0-6) plus a charging flag (bit 7).
//
// Windows cannot obtain these keys (it pairs over classic Bluetooth only). On a Mac they are the
// `MagicAccIRK` and `MagicAccEncKey` values of the `MobileBluetooth` Keychain item. Keys are taken
// exactly as macOS stores them; macOS stores the IRK least significant octet first.

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Core::AppleCP {

using Key128 = std::array<uint8_t, 16>;
using Block128 = std::array<uint8_t, 16>;

// Minimal AES-128 (FIPS-197) for the two single-block operations above. Not constant time, which
// does not matter here: both inputs are broadcast in the clear by the AirPods themselves.
namespace Aes128 {
Block128 Encrypt(const Key128 &key, const Block128 &plaintext);
Block128 Decrypt(const Key128 &key, const Block128 &ciphertext);
} // namespace Aes128

struct ProximityKeys {
    Key128 irk{};                 // as stored by macOS (least significant octet first)
    std::optional<Key128> encKey; // as stored by macOS

    bool operator==(const ProximityKeys &) const = default;
};

// Accepts, case-insensitively and ignoring spaces, colons and dashes inside the hex:
//   - "IRK=<32 hex>" and optionally "ENC=<32 hex>" on separate lines or separated by ';' or ','
//   - "MagicAccIRK ... MagicAccEncKey ..." as copied from the Keychain XML
//   - "librepods://add-magic-keys?irk=<hex>&enc_key=<hex>"
//   - "<32 hex>" or "<32 hex> <32 hex>" (IRK first)
// Returns nullopt when no valid IRK is found or a present encryption key is malformed.
std::optional<ProximityKeys> ParseProximityKeys(std::string_view text);

// Bluetooth Core `ah` function. `prand` is the 24-bit random part, `irk` in macOS octet order.
uint32_t AddressHash(const Key128 &irk, uint32_t prand);

// True when the 48-bit `address` (as Windows reports it: the most significant octet is the first
// one printed) is a resolvable private address generated with `irk`. Also accepts the address in
// reversed octet order, which some stacks report.
bool ResolvesAddress(const Key128 &irk, uint64_t address);

struct DecryptedBattery {
    std::optional<uint8_t> percent; // 0-100, nullopt when the value is out of range
    bool charging{};
};

struct DecryptedStatus {
    DecryptedBattery broadcasting; // the pod that sent this advertisement
    DecryptedBattery other;
    DecryptedBattery caseBox;
};

// Decrypts the trailing block of a proximity pairing message.
DecryptedStatus DecryptStatus(const Key128 &encKey, const Block128 &encrypted);

} // namespace Core::AppleCP
