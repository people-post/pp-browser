#pragma once

namespace pbr::os {

/** The device's face / finger sensor with something enrolled (at-rest A014). */
enum class Biometry { None, Face, Touch, Fingerprint };

/**
 * What the biometric unlock switch is called, and whether it shows at all. iOS: LAContext biometryType when
 * LAPolicyDeviceOwnerAuthenticationWithBiometrics can be evaluated (enrolled, not locked out of the policy); None
 * elsewhere (Android's fingerprint comes in its own PR). Shows no prompt.
 */
Biometry BiometryKind();

} // namespace pbr::os
