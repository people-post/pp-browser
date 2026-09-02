#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include "foundation/platform/os/OsTlsPlatformCurl.h"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <curl/curl.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace pbr::os {
namespace {

bool AppendX509AsSecCertificate(CFMutableArrayRef certs, X509* x509) {
  if (!certs || !x509) {
    return false;
  }
  unsigned char* der = nullptr;
  const int der_len = i2d_X509(x509, &der);
  if (der_len <= 0 || !der) {
    return false;
  }
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, der, der_len);
  OPENSSL_free(der);
  if (!data) {
    return false;
  }
  SecCertificateRef cert = SecCertificateCreateWithData(kCFAllocatorDefault, data);
  CFRelease(data);
  if (!cert) {
    return false;
  }
  CFArrayAppendValue(certs, cert);
  CFRelease(cert);
  return true;
}

int AppleCertVerifyCallback(X509_STORE_CTX* store_ctx, void* /*arg*/) {
  if (!store_ctx) {
    return 0;
  }

  X509* leaf = X509_STORE_CTX_get0_cert(store_ctx);
  if (!leaf) {
    return 0;
  }

  CFMutableArrayRef certs = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
  if (!certs) {
    return 0;
  }

  if (!AppendX509AsSecCertificate(certs, leaf)) {
    CFRelease(certs);
    return 0;
  }

  if (STACK_OF(X509)* chain = X509_STORE_CTX_get0_untrusted(store_ctx)) {
    const int count = sk_X509_num(chain);
    for (int i = 0; i < count; ++i) {
      X509* intermediate = sk_X509_value(chain, i);
      if (!intermediate || X509_cmp(intermediate, leaf) == 0) {
        continue;
      }
      if (!AppendX509AsSecCertificate(certs, intermediate)) {
        CFRelease(certs);
        return 0;
      }
    }
  }

  CFStringRef host = nullptr;
  if (SSL* ssl = static_cast<SSL*>(
          X509_STORE_CTX_get_ex_data(store_ctx, SSL_get_ex_data_X509_STORE_CTX_idx()))) {
    if (const char* sni = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name)) {
      if (sni[0] != '\0') {
        host = CFStringCreateWithCString(kCFAllocatorDefault, sni, kCFStringEncodingUTF8);
      }
    }
  }

  SecPolicyRef policy = SecPolicyCreateSSL(true, host);
  if (host) {
    CFRelease(host);
  }
  if (!policy) {
    CFRelease(certs);
    return 0;
  }

  SecTrustRef trust = nullptr;
  const OSStatus status = SecTrustCreateWithCertificates(certs, policy, &trust);
  CFRelease(policy);
  CFRelease(certs);
  if (status != errSecSuccess || !trust) {
    return 0;
  }

  CFErrorRef error = nullptr;
  const bool ok = SecTrustEvaluateWithError(trust, &error);
  if (error) {
    CFRelease(error);
  }
  CFRelease(trust);
  return ok ? 1 : 0;
}

CURLcode SslCtxFunction(CURL* /*curl*/, void* ssl_ctx, void* /*userptr*/) {
  if (!ssl_ctx) {
    return CURLE_BAD_FUNCTION_ARGUMENT;
  }
  SSL_CTX_set_cert_verify_callback(static_cast<SSL_CTX*>(ssl_ctx), AppleCertVerifyCallback, nullptr);
  return CURLE_OK;
}

} // namespace

void ApplyPlatformCurlSsl(CURL* curl) {
  if (!curl) {
    return;
  }
  // BoringSSL has an empty trust store on iOS; SecTrust uses the system roots
  // (and MDM/user anchors) including Apple's live distrust decisions.
  curl_easy_setopt(curl, CURLOPT_SSL_CTX_FUNCTION, SslCtxFunction);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
}

} // namespace pbr::os

#endif // __APPLE__ && TARGET_OS_IPHONE
