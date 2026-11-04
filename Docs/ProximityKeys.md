# Proximity keys (IRK and encryption key)

AirPods advertise over Bluetooth LE with resolvable private addresses that rotate. Without keys,
AirPodsDesktop can only guess which advertisements belong to the bound AirPods (same model, similar
battery, similar RSSI), so another pair of the same model nearby can be mistaken for yours.

Every Apple device the AirPods are paired with holds two 16-byte keys for them:

| Key | Keychain name | Effect in AirPodsDesktop |
|---|---|---|
| Identity Resolving Key (IRK) | `MagicAccIRK` | Only advertisements whose address resolves with it are accepted; the heuristics and the RSSI floor are skipped for them |
| Proximity encryption key | `MagicAccEncKey` | Decrypts the last 16 bytes of the advertisement: battery of each pod and the case in 1 % steps instead of 10 % |

The keys are the same on every device the AirPods are paired with. Windows cannot obtain them: it
pairs over classic Bluetooth only, and user-mode programs cannot open the L2CAP channel the AirPods
use to hand them out.

## Getting the keys from a Mac

The Mac must have connected to the AirPods at least once (appearing through iCloud is not enough).

1. Open **Keychain Access** and search for `MobileBluetooth`.
2. Open the item whose account is the AirPods' Bluetooth address and tick **Show password**. macOS
   asks for the user password, and possibly to allow access; this needs someone at the Mac, it
   cannot be done over SSH (`security` exits with 36).
3. Copy the values of `MagicAccIRK` and `MagicAccEncKey`.

## Entering them

Settings › Features › **Proximity keys**. Accepted formats, case-insensitive, with or without `:`,
`-` or `0x` inside the hex:

- `IRK=<32 hex> ENC=<32 hex>` (any separator, any order)
- the XML copied from Keychain Access
- `librepods://add-magic-keys?irk=<hex>&enc_key=<hex>`
- `<irk hex> <enc hex>`, or the IRK alone

The status line under the field says what was recognized. The value is stored like the bound device
address (marked sensitive, never written to the log); the log only records whether each key is
present.

## Implementation

`Source/Core/AppleProximityKeys.*` (domain layer, no platform calls): a minimal AES-128, the
Bluetooth Core `ah` address hash, address resolution (also in reversed octet order) and the
decryption of the status block. Tests in `Tests/ProximityKeysTests.cpp` use the FIPS-197 and
Bluetooth Core test vectors plus a real AirPods Pro advertisement with its keys from the CAPod test
suite; `Tests/AirPodsDomainTests.cpp` covers the state manager with and without keys.
