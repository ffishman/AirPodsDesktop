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

#include "AppleProximityKeys.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace Core::AppleCP {

//////////////////////////////////////////////////
// AES-128
//

namespace Aes128 {
namespace {

constexpr uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

struct InverseSbox {
    uint8_t table[256]{};
    constexpr InverseSbox()
    {
        for (int i = 0; i < 256; ++i) {
            table[kSbox[i]] = static_cast<uint8_t>(i);
        }
    }
};
constexpr InverseSbox kInverseSbox{};

constexpr uint8_t XTime(uint8_t x)
{
    return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00));
}

constexpr uint8_t Multiply(uint8_t x, uint8_t y)
{
    uint8_t result = 0;
    while (y != 0) {
        if (y & 1) {
            result ^= x;
        }
        x = XTime(x);
        y >>= 1;
    }
    return result;
}

using RoundKeys = std::array<Block128, 11>;

RoundKeys ExpandKey(const Key128 &key)
{
    // Words are stored column by column, so round key r is words [4r, 4r + 3].
    uint8_t words[44][4];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            words[i][j] = key[4 * i + j];
        }
    }

    uint8_t rcon = 0x01;
    for (int i = 4; i < 44; ++i) {
        uint8_t temp[4] = {words[i - 1][0], words[i - 1][1], words[i - 1][2], words[i - 1][3]};
        if (i % 4 == 0) {
            const uint8_t first = temp[0];
            temp[0] = static_cast<uint8_t>(kSbox[temp[1]] ^ rcon);
            temp[1] = kSbox[temp[2]];
            temp[2] = kSbox[temp[3]];
            temp[3] = kSbox[first];
            rcon = XTime(rcon);
        }
        for (int j = 0; j < 4; ++j) {
            words[i][j] = static_cast<uint8_t>(words[i - 4][j] ^ temp[j]);
        }
    }

    RoundKeys roundKeys{};
    for (int r = 0; r < 11; ++r) {
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                roundKeys[r][4 * i + j] = words[4 * r + i][j];
            }
        }
    }
    return roundKeys;
}

// The state is kept in input order: byte `4 * column + row`.
void AddRoundKey(Block128 &state, const Block128 &roundKey)
{
    for (size_t i = 0; i < 16; ++i) {
        state[i] ^= roundKey[i];
    }
}

void SubBytes(Block128 &state)
{
    for (auto &b : state) {
        b = kSbox[b];
    }
}

void InvSubBytes(Block128 &state)
{
    for (auto &b : state) {
        b = kInverseSbox.table[b];
    }
}

void ShiftRows(Block128 &state)
{
    Block128 out;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out[4 * c + r] = state[4 * ((c + r) % 4) + r];
        }
    }
    state = out;
}

void InvShiftRows(Block128 &state)
{
    Block128 out;
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out[4 * ((c + r) % 4) + r] = state[4 * c + r];
        }
    }
    state = out;
}

void MixColumns(Block128 &state)
{
    for (int c = 0; c < 4; ++c) {
        uint8_t *col = &state[4 * c];
        const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = static_cast<uint8_t>(XTime(a0) ^ (XTime(a1) ^ a1) ^ a2 ^ a3);
        col[1] = static_cast<uint8_t>(a0 ^ XTime(a1) ^ (XTime(a2) ^ a2) ^ a3);
        col[2] = static_cast<uint8_t>(a0 ^ a1 ^ XTime(a2) ^ (XTime(a3) ^ a3));
        col[3] = static_cast<uint8_t>((XTime(a0) ^ a0) ^ a1 ^ a2 ^ XTime(a3));
    }
}

void InvMixColumns(Block128 &state)
{
    for (int c = 0; c < 4; ++c) {
        uint8_t *col = &state[4 * c];
        const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = static_cast<uint8_t>(
            Multiply(a0, 0x0e) ^ Multiply(a1, 0x0b) ^ Multiply(a2, 0x0d) ^ Multiply(a3, 0x09));
        col[1] = static_cast<uint8_t>(
            Multiply(a0, 0x09) ^ Multiply(a1, 0x0e) ^ Multiply(a2, 0x0b) ^ Multiply(a3, 0x0d));
        col[2] = static_cast<uint8_t>(
            Multiply(a0, 0x0d) ^ Multiply(a1, 0x09) ^ Multiply(a2, 0x0e) ^ Multiply(a3, 0x0b));
        col[3] = static_cast<uint8_t>(
            Multiply(a0, 0x0b) ^ Multiply(a1, 0x0d) ^ Multiply(a2, 0x09) ^ Multiply(a3, 0x0e));
    }
}

} // namespace

Block128 Encrypt(const Key128 &key, const Block128 &plaintext)
{
    const auto roundKeys = ExpandKey(key);
    Block128 state = plaintext;

    AddRoundKey(state, roundKeys[0]);
    for (int round = 1; round < 10; ++round) {
        SubBytes(state);
        ShiftRows(state);
        MixColumns(state);
        AddRoundKey(state, roundKeys[round]);
    }
    SubBytes(state);
    ShiftRows(state);
    AddRoundKey(state, roundKeys[10]);
    return state;
}

Block128 Decrypt(const Key128 &key, const Block128 &ciphertext)
{
    const auto roundKeys = ExpandKey(key);
    Block128 state = ciphertext;

    AddRoundKey(state, roundKeys[10]);
    for (int round = 9; round > 0; --round) {
        InvShiftRows(state);
        InvSubBytes(state);
        AddRoundKey(state, roundKeys[round]);
        InvMixColumns(state);
    }
    InvShiftRows(state);
    InvSubBytes(state);
    AddRoundKey(state, roundKeys[0]);
    return state;
}

} // namespace Aes128

//////////////////////////////////////////////////
// Key parsing
//

namespace {

int HexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool IsKeyChar(char c)
{
    // Hex digits plus the separators commonly used inside a printed key.
    return HexValue(c) >= 0 || c == ':' || c == '-';
}

struct Candidate {
    size_t position;
    Key128 key;
};

// Every run of key characters that contains exactly 32 hex digits.
std::vector<Candidate> FindKeyCandidates(std::string_view text)
{
    std::vector<Candidate> candidates;

    size_t i = 0;
    while (i < text.size()) {
        if (!IsKeyChar(text[i])) {
            ++i;
            continue;
        }

        const size_t start = i;
        std::string digits;
        while (i < text.size() && IsKeyChar(text[i])) {
            if (HexValue(text[i]) >= 0) {
                digits.push_back(text[i]);
            }
            ++i;
        }

        if (digits.size() != 32) {
            continue;
        }

        Key128 key{};
        for (size_t b = 0; b < 16; ++b) {
            key[b] =
                static_cast<uint8_t>(HexValue(digits[2 * b]) << 4 | HexValue(digits[2 * b + 1]));
        }
        candidates.push_back({start, key});
    }

    return candidates;
}

enum class Label : uint32_t { None, Irk, Enc };

// A label only counts when it is written right before the key, as in "IRK=<hex>" or
// "<key>MagicAccIRK</key><string><hex>".
constexpr size_t kLabelWindow = 48;

// The label written closest before `position`, if any.
Label LabelBefore(const std::string &lower, size_t position)
{
    const size_t from = position > kLabelWindow ? position - kLabelWindow : 0;
    const auto before = lower.substr(from, position - from);
    const auto irk = before.rfind("irk");
    const auto enc = before.rfind("enc");

    if (irk == std::string::npos && enc == std::string::npos) {
        return Label::None;
    }
    if (enc == std::string::npos) {
        return Label::Irk;
    }
    if (irk == std::string::npos) {
        return Label::Enc;
    }
    return irk > enc ? Label::Irk : Label::Enc;
}

} // namespace

std::optional<ProximityKeys> ParseProximityKeys(std::string_view text)
{
    std::string lower{text};
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    // "encoding" (XML declaration) is not an encryption key label.
    for (size_t pos = lower.find("encoding"); pos != std::string::npos;
         pos = lower.find("encoding", pos))
    {
        lower.replace(pos, 8, 8, ' ');
    }

    // Drop "0x" prefixes so they do not glue onto the digits that follow.
    for (size_t pos = lower.find("0x"); pos != std::string::npos; pos = lower.find("0x", pos)) {
        lower[pos] = ' ';
        lower[pos + 1] = ' ';
    }

    const auto candidates = FindKeyCandidates(lower);
    if (candidates.empty()) {
        return std::nullopt;
    }

    std::optional<Key128> irk, enc;
    std::vector<Key128> unlabeled;
    for (const auto &candidate : candidates) {
        switch (LabelBefore(lower, candidate.position)) {
        case Label::Irk:
            if (!irk.has_value()) {
                irk = candidate.key;
            }
            break;
        case Label::Enc:
            if (!enc.has_value()) {
                enc = candidate.key;
            }
            break;
        case Label::None:
            unlabeled.push_back(candidate.key);
            break;
        }
    }

    // Unlabeled keys fill the gaps in order: IRK first, then the encryption key.
    for (const auto &key : unlabeled) {
        if (!irk.has_value()) {
            irk = key;
        }
        else if (!enc.has_value()) {
            enc = key;
        }
    }

    if (!irk.has_value()) {
        return std::nullopt;
    }
    return ProximityKeys{*irk, enc};
}

//////////////////////////////////////////////////
// Address resolution
//

uint32_t AddressHash(const Key128 &irk, uint32_t prand)
{
    // The Core specification writes keys and data most significant octet first; macOS stores the
    // IRK the other way round.
    Key128 key;
    std::reverse_copy(irk.begin(), irk.end(), key.begin());

    Block128 plaintext{};
    plaintext[13] = static_cast<uint8_t>(prand >> 16);
    plaintext[14] = static_cast<uint8_t>(prand >> 8);
    plaintext[15] = static_cast<uint8_t>(prand);

    const auto ciphertext = Aes128::Encrypt(key, plaintext);
    return static_cast<uint32_t>(ciphertext[13]) << 16 |
           static_cast<uint32_t>(ciphertext[14]) << 8 | static_cast<uint32_t>(ciphertext[15]);
}

namespace {

bool ResolvesInOrder(const Key128 &irk, uint64_t address)
{
    // A resolvable private address has 0b01 in its two most significant bits.
    if (((address >> 46) & 0b11) != 0b01) {
        return false;
    }
    const auto prand = static_cast<uint32_t>((address >> 24) & 0xFFFFFF);
    const auto hash = static_cast<uint32_t>(address & 0xFFFFFF);
    return AddressHash(irk, prand) == hash;
}

uint64_t ReverseOctets(uint64_t address)
{
    uint64_t reversed = 0;
    for (int i = 0; i < 6; ++i) {
        reversed = (reversed << 8) | ((address >> (8 * i)) & 0xFF);
    }
    return reversed;
}

} // namespace

bool ResolvesAddress(const Key128 &irk, uint64_t address)
{
    address &= 0xFFFFFFFFFFFFull;
    return ResolvesInOrder(irk, address) || ResolvesInOrder(irk, ReverseOctets(address));
}

//////////////////////////////////////////////////
// Decryption
//

namespace {

DecryptedBattery ToBattery(uint8_t raw)
{
    DecryptedBattery battery;
    const uint8_t percent = raw & 0x7F;
    if (percent <= 100) {
        battery.percent = percent;
    }
    battery.charging = (raw & 0x80) != 0;
    return battery;
}

} // namespace

DecryptedStatus DecryptStatus(const Key128 &encKey, const Block128 &encrypted)
{
    const auto plain = Aes128::Decrypt(encKey, encrypted);
    return DecryptedStatus{ToBattery(plain[1]), ToBattery(plain[2]), ToBattery(plain[3])};
}

} // namespace Core::AppleCP
