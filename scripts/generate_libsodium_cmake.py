#!/usr/bin/env python3
"""Regenerate third_party/libsodium/CMakeLists.txt from vendored Makefile.am."""
from __future__ import annotations

import re
import sys
from pathlib import Path

HEADER = """\
# libsodium — upstream sources imported by scripts/vendor_import.sh
# CMake glue adapted from https://github.com/robinlinden/libsodium-cmake (ISC)

cmake_minimum_required(VERSION 3.24)

project(sodium LANGUAGES C)

option(SODIUM_MINIMAL "Only compile the minimum set of functions required for the high-level API" OFF)
option(SODIUM_ENABLE_BLOCKING_RANDOM "Enable this switch only if /dev/urandom is totally broken on the target platform" OFF)

set(VERSION 1.0.20)
set(SODIUM_LIBRARY_VERSION_MAJOR 26)
set(SODIUM_LIBRARY_VERSION_MINOR 2)
if(SODIUM_MINIMAL)
  set(SODIUM_LIBRARY_MINIMAL_DEF "#define SODIUM_LIBRARY_MINIMAL 1")
endif()

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/libsodium/include/sodium/version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/sodium/version.h
)

"""

TAIL = """
set_target_properties(${PROJECT_NAME}
  PROPERTIES
    C_STANDARD 99
)

target_include_directories(${PROJECT_NAME}
  PUBLIC
    src/libsodium/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
  PRIVATE
    src/libsodium/include/sodium
    ${CMAKE_CURRENT_BINARY_DIR}/generated/sodium
)

target_compile_definitions(${PROJECT_NAME}
  PUBLIC
    $<$<NOT:$<BOOL:${BUILD_SHARED_LIBS}>>:SODIUM_STATIC>
    $<$<BOOL:${SODIUM_MINIMAL}>:SODIUM_LIBRARY_MINIMAL>
  PRIVATE
    CONFIGURED
    $<$<BOOL:${BUILD_SHARED_LIBS}>:SODIUM_DLL_EXPORT>
    $<$<BOOL:${SODIUM_ENABLE_BLOCKING_RANDOM}>:USE_BLOCKING_RANDOM>
    $<$<BOOL:${SODIUM_MINIMAL}>:MINIMAL>
    $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
)

if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  target_compile_definitions(${PROJECT_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(${PROJECT_NAME}
    PUBLIC -mavx2
    PRIVATE -maes -mpclmul -mssse3
  )
endif()

if(NOT TARGET sodium::sodium)
  add_library(sodium::sodium ALIAS sodium)
endif()
"""

# Headers pulled into the static archive (from robinlinden/libsodium-cmake).
HEADER_SOURCES = [
    "src/libsodium/crypto_core/ed25519/ref10/fe_25_5/base.h",
    "src/libsodium/crypto_core/ed25519/ref10/fe_25_5/base2.h",
    "src/libsodium/crypto_core/ed25519/ref10/fe_25_5/constants.h",
    "src/libsodium/crypto_core/ed25519/ref10/fe_25_5/fe.h",
    "src/libsodium/crypto_core/ed25519/ref10/fe_51/base.h",
    "src/libsodium/crypto_core/ed25519/ref10/fe_51/base2.h",
    "src/libsodium/crypto_core/ed25519/ref10/fe_51/constants.h",
    "src/libsodium/crypto_core/ed25519/ref10/fe_51/fe.h",
    "src/libsodium/crypto_generichash/blake2b/ref/blake2.h",
    "src/libsodium/crypto_generichash/blake2b/ref/blake2b-compress-avx2.h",
    "src/libsodium/crypto_generichash/blake2b/ref/blake2b-compress-sse41.h",
    "src/libsodium/crypto_generichash/blake2b/ref/blake2b-compress-ssse3.h",
    "src/libsodium/crypto_generichash/blake2b/ref/blake2b-load-avx2.h",
    "src/libsodium/crypto_generichash/blake2b/ref/blake2b-load-sse2.h",
    "src/libsodium/crypto_generichash/blake2b/ref/blake2b-load-sse41.h",
    "src/libsodium/crypto_onetimeauth/poly1305/donna/poly1305_donna.h",
    "src/libsodium/crypto_onetimeauth/poly1305/donna/poly1305_donna32.h",
    "src/libsodium/crypto_onetimeauth/poly1305/donna/poly1305_donna64.h",
    "src/libsodium/crypto_onetimeauth/poly1305/onetimeauth_poly1305.h",
    "src/libsodium/crypto_onetimeauth/poly1305/sse2/poly1305_sse2.h",
    "src/libsodium/crypto_pwhash/argon2/argon2-core.h",
    "src/libsodium/crypto_pwhash/argon2/argon2-encoding.h",
    "src/libsodium/crypto_pwhash/argon2/argon2.h",
    "src/libsodium/crypto_pwhash/argon2/blake2b-long.h",
    "src/libsodium/crypto_pwhash/argon2/blamka-round-avx2.h",
    "src/libsodium/crypto_pwhash/argon2/blamka-round-avx512f.h",
    "src/libsodium/crypto_pwhash/argon2/blamka-round-ref.h",
    "src/libsodium/crypto_pwhash/argon2/blamka-round-ssse3.h",
    "src/libsodium/crypto_scalarmult/curve25519/ref10/x25519_ref10.h",
    "src/libsodium/crypto_scalarmult/curve25519/sandy2x/consts_namespace.h",
    "src/libsodium/crypto_scalarmult/curve25519/sandy2x/curve25519_sandy2x.h",
    "src/libsodium/crypto_scalarmult/curve25519/sandy2x/fe.h",
    "src/libsodium/crypto_scalarmult/curve25519/sandy2x/fe51.h",
    "src/libsodium/crypto_scalarmult/curve25519/sandy2x/fe51_namespace.h",
    "src/libsodium/crypto_scalarmult/curve25519/sandy2x/ladder.h",
    "src/libsodium/crypto_scalarmult/curve25519/sandy2x/ladder_namespace.h",
    "src/libsodium/crypto_scalarmult/curve25519/scalarmult_curve25519.h",
    "src/libsodium/crypto_sign/ed25519/ref10/sign_ed25519_ref10.h",
    "src/libsodium/crypto_shorthash/siphash24/ref/shorthash_siphash_ref.h",
    "src/libsodium/crypto_stream/chacha20/dolbeau/chacha20_dolbeau-avx2.h",
    "src/libsodium/crypto_stream/chacha20/dolbeau/chacha20_dolbeau-ssse3.h",
    "src/libsodium/crypto_stream/chacha20/dolbeau/u0.h",
    "src/libsodium/crypto_stream/chacha20/dolbeau/u1.h",
    "src/libsodium/crypto_stream/chacha20/dolbeau/u4.h",
    "src/libsodium/crypto_stream/chacha20/dolbeau/u8.h",
    "src/libsodium/crypto_stream/chacha20/ref/chacha20_ref.h",
    "src/libsodium/crypto_stream/chacha20/stream_chacha20.h",
    "src/libsodium/crypto_stream/salsa20/ref/salsa20_ref.h",
    "src/libsodium/crypto_stream/salsa20/stream_salsa20.h",
    "src/libsodium/crypto_stream/salsa20/xmm6/salsa20_xmm6.h",
    "src/libsodium/crypto_stream/salsa20/xmm6int/salsa20_xmm6int-avx2.h",
    "src/libsodium/crypto_stream/salsa20/xmm6int/salsa20_xmm6int-sse2.h",
    "src/libsodium/crypto_stream/salsa20/xmm6int/u0.h",
    "src/libsodium/crypto_stream/salsa20/xmm6int/u1.h",
    "src/libsodium/crypto_stream/salsa20/xmm6int/u4.h",
    "src/libsodium/crypto_stream/salsa20/xmm6int/u8.h",
]

MINIMAL_EXTRA = [
    "src/libsodium/crypto_box/curve25519xchacha20poly1305/box_curve25519xchacha20poly1305.c",
    "src/libsodium/crypto_box/curve25519xchacha20poly1305/box_seal_curve25519xchacha20poly1305.c",
    "src/libsodium/crypto_core/ed25519/core_ed25519.c",
    "src/libsodium/crypto_core/ed25519/core_ristretto255.c",
    "src/libsodium/crypto_pwhash/scryptsalsa208sha256/crypto_scrypt-common.c",
    "src/libsodium/crypto_pwhash/scryptsalsa208sha256/crypto_scrypt.h",
    "src/libsodium/crypto_pwhash/scryptsalsa208sha256/nosse/pwhash_scryptsalsa208sha256_nosse.c",
    "src/libsodium/crypto_pwhash/scryptsalsa208sha256/pbkdf2-sha256.c",
    "src/libsodium/crypto_pwhash/scryptsalsa208sha256/pbkdf2-sha256.h",
    "src/libsodium/crypto_pwhash/scryptsalsa208sha256/pwhash_scryptsalsa208sha256.c",
    "src/libsodium/crypto_pwhash/scryptsalsa208sha256/scrypt_platform.c",
    "src/libsodium/crypto_pwhash/scryptsalsa208sha256/sse/pwhash_scryptsalsa208sha256_sse.c",
    "src/libsodium/crypto_scalarmult/ed25519/ref10/scalarmult_ed25519_ref10.c",
    "src/libsodium/crypto_scalarmult/ristretto255/ref10/scalarmult_ristretto255_ref10.c",
    "src/libsodium/crypto_secretbox/xchacha20poly1305/secretbox_xchacha20poly1305.c",
    "src/libsodium/crypto_shorthash/siphash24/ref/shorthash_siphashx24_ref.c",
    "src/libsodium/crypto_shorthash/siphash24/shorthash_siphashx24.c",
    "src/libsodium/crypto_sign/ed25519/ref10/obsolete.c",
    "src/libsodium/crypto_stream/salsa2012/ref/stream_salsa2012_ref.c",
    "src/libsodium/crypto_stream/salsa2012/stream_salsa2012.c",
    "src/libsodium/crypto_stream/salsa208/ref/stream_salsa208_ref.c",
    "src/libsodium/crypto_stream/salsa208/stream_salsa208.c",
    "src/libsodium/crypto_stream/xchacha20/stream_xchacha20.c",
]

# Optional accelerated implementations (runtime-selected).
OPTIONAL_SOURCES = [
    "src/libsodium/crypto_aead/aegis128l/aegis128l_aesni.c",
    "src/libsodium/crypto_aead/aegis128l/aegis128l_armcrypto.c",
    "src/libsodium/crypto_aead/aegis256/aegis256_aesni.c",
    "src/libsodium/crypto_aead/aegis256/aegis256_armcrypto.c",
    "src/libsodium/crypto_aead/aes256gcm/aesni/aead_aes256gcm_aesni.c",
    "src/libsodium/crypto_aead/aes256gcm/armcrypto/aead_aes256gcm_armcrypto.c",
    "src/libsodium/crypto_generichash/blake2b/ref/blake2b-compress-avx2.c",
    "src/libsodium/crypto_generichash/blake2b/ref/blake2b-compress-sse41.c",
    "src/libsodium/crypto_generichash/blake2b/ref/blake2b-compress-ssse3.c",
    "src/libsodium/crypto_onetimeauth/poly1305/sse2/poly1305_sse2.c",
    "src/libsodium/crypto_pwhash/argon2/argon2-fill-block-avx2.c",
    "src/libsodium/crypto_pwhash/argon2/argon2-fill-block-avx512f.c",
    "src/libsodium/crypto_pwhash/argon2/argon2-fill-block-ssse3.c",
    "src/libsodium/crypto_stream/chacha20/dolbeau/chacha20_dolbeau-avx2.c",
    "src/libsodium/crypto_stream/chacha20/dolbeau/chacha20_dolbeau-ssse3.c",
    "src/libsodium/crypto_stream/salsa20/xmm6/salsa20_xmm6.c",
    "src/libsodium/crypto_stream/salsa20/xmm6int/salsa20_xmm6int-avx2.c",
    "src/libsodium/crypto_stream/salsa20/xmm6int/salsa20_xmm6int-sse2.c",
]


def parse_makefile_sources(makefile: Path) -> list[str]:
    lines = makefile.read_text().splitlines()
    sources: list[str] = []
    in_block = False
    for line in lines:
        if line.startswith("libsodium_la_SOURCES"):
            in_block = True
            continue
        if not in_block:
            continue
        stripped = line.strip()
        if not stripped:
            break
        sources.append(stripped.rstrip("\\").strip())
        if not line.rstrip().endswith("\\"):
            break
    return [f"src/libsodium/{s}" for s in sources if s.endswith(".c")]


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("third_party/libsodium")
    makefile = root / "src/libsodium/Makefile.am"
    if not makefile.is_file():
        print(f"error: missing {makefile}", file=sys.stderr)
        return 1

    sources = parse_makefile_sources(makefile)
    for path in OPTIONAL_SOURCES:
        if (root / path).is_file():
            sources.append(path)

    public_headers = sorted(
        str(p.relative_to(root))
        for p in (root / "src/libsodium/include").rglob("*")
        if p.is_file() and p.name != "version.h.in" and p.name != "version.h"
    )

    all_sources = sorted(set(sources + HEADER_SOURCES + public_headers))

    body = ["add_library(${PROJECT_NAME}"]
    body.extend(f"  {path}" for path in all_sources)
    body.append(")")
    body.append("")
    body.append("if(NOT SODIUM_MINIMAL)")
    body.append("  target_sources(${PROJECT_NAME} PRIVATE")
    for path in MINIMAL_EXTRA:
        body.append(f"    {path}")
    body.append("  )")
    body.append("endif()")
    body.append("")

    out = root / "CMakeLists.txt"
    out.write_text(HEADER + "\n".join(body) + TAIL)
    print(f"Wrote {out} ({len(all_sources)} base sources)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
