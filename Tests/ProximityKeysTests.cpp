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

#include <QtTest>

#include <algorithm>
#include <string>

#include "Source/Core/AppleProximityKeys.h"

using namespace Core::AppleCP;

namespace {

std::array<uint8_t, 16> FromHex(const char *hex)
{
    std::array<uint8_t, 16> out{};
    std::string digits;
    for (const char *p = hex; *p != '\0'; ++p) {
        if (std::isxdigit(static_cast<unsigned char>(*p))) {
            digits.push_back(*p);
        }
    }
    for (size_t i = 0; i < 16; ++i) {
        out[i] = static_cast<uint8_t>(std::stoi(digits.substr(2 * i, 2), nullptr, 16));
    }
    return out;
}

Key128 Reversed(Key128 key)
{
    std::reverse(key.begin(), key.end());
    return key;
}

// A real AirPods Pro advertisement with its keys, from the CAPod test suite
// (AirPodsProTest "decrypt data"): left 98 %, right 100 % and charging, case 86 %.
constexpr uint64_t kSampleAddress = 0x5A162B91D1CD;
constexpr auto kSampleIrk = "79-04-65-1E-E2-CC-D9-26-F2-6E-20-EE-3E-CC-DE-79";
constexpr auto kSampleEncKey = "3B-9C-80-57-E6-45-7F-F2-1B-8E-07-63-6C-99-E0-29";
constexpr auto kSampleEncrypted = "0C 14 E0 EB 43 3F 4B 22 C0 A9 ED CB 33 E7 09 71";

} // namespace

class ProximityKeysTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void aesMatchesFips197();
    void addressHashMatchesCoreSpecification();
    void resolvesASampleAddress();
    void rejectsOtherKeysAndNonResolvableAddresses();
    void resolvesReversedOctetOrder();
    void decryptsSampleBattery();
    void parsesLabeledLines();
    void parsesLibrePodsUri();
    void parsesKeychainXml();
    void parsesBareKeys();
    void rejectsMalformedInput();
};

void ProximityKeysTests::aesMatchesFips197()
{
    // FIPS-197 Appendix C.1
    const auto key = FromHex("000102030405060708090a0b0c0d0e0f");
    const auto plain = FromHex("00112233445566778899aabbccddeeff");
    const auto cipher = FromHex("69c4e0d86a7b0430d8cdb78070b4c55a");

    QVERIFY(Aes128::Encrypt(key, plain) == cipher);
    QVERIFY(Aes128::Decrypt(key, cipher) == plain);
}

void ProximityKeysTests::addressHashMatchesCoreSpecification()
{
    // Bluetooth Core Specification, Vol 3, Part H, D.7 (ah random address hash function). The
    // specification writes the IRK most significant octet first; the API takes macOS order.
    const auto irk = Reversed(FromHex("ec0234a357c8ad05341010a60a397d9b"));
    QCOMPARE(AddressHash(irk, 0x708194), uint32_t{0x0dfbaa});
}

void ProximityKeysTests::resolvesASampleAddress()
{
    QVERIFY(ResolvesAddress(FromHex(kSampleIrk), kSampleAddress));
}

void ProximityKeysTests::rejectsOtherKeysAndNonResolvableAddresses()
{
    QVERIFY(!ResolvesAddress(
        FromHex("79-04-65-1E-E2-CC-D9-26-F2-6E-20-EE-3E-CC-DE-AA"), kSampleAddress));
    // Same hash and prand bits, but not marked as resolvable (top bits 0b00).
    QVERIFY(!ResolvesAddress(FromHex(kSampleIrk), kSampleAddress & 0x3FFFFFFFFFFF));
    QVERIFY(!ResolvesAddress(FromHex(kSampleIrk), kSampleAddress ^ 0x1));
}

void ProximityKeysTests::resolvesReversedOctetOrder()
{
    QVERIFY(ResolvesAddress(FromHex(kSampleIrk), 0xCDD1912B165A));
}

void ProximityKeysTests::decryptsSampleBattery()
{
    const auto status = DecryptStatus(FromHex(kSampleEncKey), FromHex(kSampleEncrypted));

    // The sample was broadcast by the right pod.
    QCOMPARE(status.broadcasting.percent, std::optional<uint8_t>{100});
    QVERIFY(status.broadcasting.charging);
    QCOMPARE(status.other.percent, std::optional<uint8_t>{98});
    QVERIFY(!status.other.charging);
    QCOMPARE(status.caseBox.percent, std::optional<uint8_t>{86});
    QVERIFY(!status.caseBox.charging);
}

void ProximityKeysTests::parsesLabeledLines()
{
    const auto keys = ParseProximityKeys(
        "IRK=7904651EE2CCD926F26E20EE3ECCDE79\nENC=3b9c8057e6457ff21b8e07636c99e029\n");
    QVERIFY(keys.has_value());
    QVERIFY(keys->irk == FromHex(kSampleIrk));
    QVERIFY(keys->encKey == FromHex(kSampleEncKey));

    // Order does not matter when labeled.
    const auto swapped = ParseProximityKeys(
        "enc: 3B-9C-80-57-E6-45-7F-F2-1B-8E-07-63-6C-99-E0-29; irk: 79:04:65:1E:E2:CC:D9:26:F2:6E:"
        "20:EE:3E:CC:DE:79");
    QVERIFY(swapped == keys);
}

void ProximityKeysTests::parsesLibrePodsUri()
{
    const auto keys = ParseProximityKeys(
        "librepods://add-magic-keys?irk=7904651EE2CCD926F26E20EE3ECCDE79&enc_key="
        "3B9C8057E6457FF21B8E07636C99E029");
    QVERIFY(keys.has_value());
    QVERIFY(keys->irk == FromHex(kSampleIrk));
    QVERIFY(keys->encKey == FromHex(kSampleEncKey));
}

void ProximityKeysTests::parsesKeychainXml()
{
    const auto keys = ParseProximityKeys(R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
	<key>MagicAccEncKey</key>
	<string>3B9C8057E6457FF21B8E07636C99E029</string>
	<key>MagicAccIRK</key>
	<string>7904651EE2CCD926F26E20EE3ECCDE79</string>
</dict>
</plist>)");
    QVERIFY(keys.has_value());
    QVERIFY(keys->irk == FromHex(kSampleIrk));
    QVERIFY(keys->encKey == FromHex(kSampleEncKey));
}

void ProximityKeysTests::parsesBareKeys()
{
    const auto both = ParseProximityKeys(
        "  0x7904651EE2CCD926F26E20EE3ECCDE79   3B9C8057E6457FF21B8E07636C99E029 ");
    QVERIFY(both.has_value());
    QVERIFY(both->irk == FromHex(kSampleIrk));
    QVERIFY(both->encKey == FromHex(kSampleEncKey));

    const auto irkOnly = ParseProximityKeys("7904651EE2CCD926F26E20EE3ECCDE79");
    QVERIFY(irkOnly.has_value());
    QVERIFY(!irkOnly->encKey.has_value());
}

void ProximityKeysTests::rejectsMalformedInput()
{
    QVERIFY(!ParseProximityKeys("").has_value());
    QVERIFY(!ParseProximityKeys("IRK=7904651EE2CCD926F26E20EE3ECCDE").has_value());     // 30 digits
    QVERIFY(!ParseProximityKeys("IRK=7904651EE2CCD926F26E20EE3ECCDE7900").has_value()); // 34
    QVERIFY(!ParseProximityKeys("ENC=3B9C8057E6457FF21B8E07636C99E029").has_value());   // no IRK
}

QTEST_GUILESS_MAIN(ProximityKeysTests)

#include "ProximityKeysTests.moc"
