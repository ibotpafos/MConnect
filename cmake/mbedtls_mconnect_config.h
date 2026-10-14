#pragma once

#define MBEDTLS_SSL_DTLS_SRTP

// Parallel DTLS certificate generation must not race on lazily built AES
// tables. The upstream ROM option keeps those tables immutable on all targets.
#define MBEDTLS_AES_ROM_TABLES

// The embedded self-test instrumentation increments shared ECP counters on
// ordinary key generation. It is not used by the plugin and is not thread-safe.
#undef MBEDTLS_SELF_TEST

// Use Mbed TLS's own synchronisation for shared crypto internals on POSIX.
#if !defined(_WIN32)
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
#endif
