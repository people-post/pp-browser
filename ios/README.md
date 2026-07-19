# iOS (Frame)

Native iOS builds use **CMake + Xcode toolchains** from the repository root (same as Android’s Gradle + NDK pattern).

| Path | Purpose |
|------|---------|
| [`packaging/ios/`](../packaging/ios/) | Info.plist, entitlements, signing placeholders |
| [`scripts/ios_build.sh`](../scripts/ios_build.sh) | Configure, build, install, run on simulator |
| [`scripts/ios_sign.sh`](../scripts/ios_sign.sh) | Device code-sign and IPA export |
| [`docs/ops/IOS_BUILD.md`](../docs/ops/IOS_BUILD.md) | Full setup guide |

Quick start (macOS):

```bash
./scripts/ios_build.sh sim
./scripts/ios_build.sh run-sim
```

Device signing placeholders: `packaging/ios/signing.env.example`
