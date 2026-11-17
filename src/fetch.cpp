#include "fetch.h"

#include <WiFiClient.h>
#include <time.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/md.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include "ca_bundle.h"
#include "config.h"
#include "net.h"

namespace fetch {
namespace {

struct Url {
  bool https;
  String host;
  uint16_t port;
  String path;
};

bool parseUrl(const String &text, Url &out) {
  out.https = text.startsWith("https://");
  if (!out.https && !text.startsWith("http://")) return false;
  int start = out.https ? 8 : 7;
  int end = text.length();
  int slash = text.indexOf('/', start);
  int query = text.indexOf('?', start);
  if (slash >= 0 && slash < end) end = slash;
  if (query >= 0 && query < end) end = query;
  String authority = text.substring(start, end);
  out.path = text.substring(end);
  if (out.path.length() == 0 || out.path[0] != '/') out.path = "/" + out.path;
  out.port = out.https ? 443 : 80;
  int colon = authority.lastIndexOf(':');
  if (colon >= 0) {
    long port = authority.substring(colon + 1).toInt();
    if (port < 1 || port > 65535) return false;
    out.port = static_cast<uint16_t>(port);
    authority = authority.substring(0, colon);
  }
  out.host = authority;
  return out.host.length() > 0;
}

uint32_t epochOf(const mbedtls_x509_time &t) {
  int y = t.year, m = t.mon;
  y -= m <= 2;
  int era = y / 400;
  int yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + t.day - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  uint32_t days = static_cast<uint32_t>(era * 146097 + doe - 719468);
  return days * 86400UL + t.hour * 3600UL + t.min * 60UL + t.sec;
}

// ---- the CA bundle ---------------------------------------------------------
// Layout: a two-byte count, then for each root a two-byte name length, a
// two-byte key length, the subject name and the public key, all DER.

// Calls `match` with the public key of every root whose name is `name`,
// until one returns true.
template <typename F>
bool anyRootNamed(const mbedtls_x509_buf &name, F match) {
  const uint8_t *p = CA_BUNDLE;
  size_t count = (p[0] << 8) | p[1];
  p += 2;
  for (size_t i = 0; i < count; i++) {
    size_t nameLen = (p[0] << 8) | p[1];
    size_t keyLen = (p[2] << 8) | p[3];
    const uint8_t *rootName = p + 4;
    const uint8_t *rootKey = rootName + nameLen;
    if (nameLen == name.len && memcmp(rootName, name.p, nameLen) == 0 && match(rootKey, keyLen)) {
      return true;
    }
    p = rootKey + keyLen;
  }
  return false;
}

bool signedWith(const mbedtls_x509_crt *crt, const uint8_t *key, size_t keyLen) {
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  unsigned char hash[MBEDTLS_MD_MAX_SIZE];
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(crt->sig_md);
  bool ok = md && mbedtls_pk_parse_public_key(&pk, key, keyLen) == 0 &&
            mbedtls_pk_can_do(&pk, crt->sig_pk) &&
            mbedtls_md(md, crt->tbs.p, crt->tbs.len, hash) == 0 &&
            mbedtls_pk_verify_ext(crt->sig_pk, crt->sig_opts, &pk, crt->sig_md, hash,
                                  mbedtls_md_get_size(md), crt->sig.p, crt->sig.len) == 0;
  mbedtls_pk_free(&pk);
  return ok;
}

// mbedTLS calls this for each certificate of the chain, top first, with what
// it found wrong. It has no trusted roots of its own here, so the top of the
// chain always arrives marked "not trusted", and this decides.
//
// The framework's mbedTLS is built without date checking, so the validity
// period of every certificate is checked here too. request() is only used
// once the clock is known to be right.
int verify(void *, mbedtls_x509_crt *crt, int, uint32_t *flags) {
  uint32_t now = static_cast<uint32_t>(time(nullptr));
  uint32_t dates = 0;
  if (now > 1700000000UL) {
    if (now > epochOf(crt->valid_to)) dates |= MBEDTLS_X509_BADCERT_EXPIRED;
    if (now < epochOf(crt->valid_from)) dates |= MBEDTLS_X509_BADCERT_FUTURE;
  }
  if (!(*flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)) {
    *flags |= dates;
    return 0;
  }
  // The certificate is one of our roots (same name, same key), whatever
  // signature the server's copy of it carries: trusted as it stands.
  bool isRoot = anyRootNamed(crt->subject_raw, [&](const uint8_t *key, size_t keyLen) {
    return keyLen == crt->pk_raw.len && memcmp(key, crt->pk_raw.p, keyLen) == 0;
  });
  if (isRoot) {
    *flags = 0;
    return 0;
  }
  // Or one of our roots signed it. Anything else wrong with it still counts.
  bool signedByRoot = anyRootNamed(crt->issuer_raw, [&](const uint8_t *key, size_t keyLen) {
    return signedWith(crt, key, keyLen);
  });
  if (signedByRoot) *flags &= ~MBEDTLS_X509_BADCERT_NOT_TRUSTED;
  *flags |= dates;
  return 0;
}

// ---- one connection --------------------------------------------------------

struct Tls {
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_entropy_context entropy;
  mbedtls_x509_crt noRoots;  // empty: verify() does the trusting
  mbedtls_net_context net;
  Tls() {
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_entropy_init(&entropy);
    mbedtls_x509_crt_init(&noRoots);
    mbedtls_net_init(&net);
  }
  ~Tls() {
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    mbedtls_x509_crt_free(&noRoots);
    // The socket belongs to the WiFiClient; `net` only borrowed its number.
  }
};

struct Conn {
  WiFiClient tcp;
  Tls *tls = nullptr;
  uint32_t deadline = 0;  // millis() by which everything must be done

  bool expired() const { return static_cast<int32_t>(millis() - deadline) >= 0; }

  bool write(const char *data, size_t len) {
    if (!tls) return tcp.write(reinterpret_cast<const uint8_t *>(data), len) == len;
    while (len > 0 && !expired()) {
      int n = mbedtls_ssl_write(&tls->ssl, reinterpret_cast<const unsigned char *>(data), len);
      if (n > 0) {
        data += n;
        len -= n;
      } else if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
        vTaskDelay(pdMS_TO_TICKS(10));
      } else {
        return false;
      }
    }
    return len == 0;
  }

  // Up to `len` bytes: > 0 read, 0 nothing yet, < 0 closed or failed.
  int read(char *out, size_t len) {
    if (!tls) {
      int n = tcp.read(reinterpret_cast<uint8_t *>(out), len);
      if (n > 0) return n;
      return tcp.connected() ? 0 : -1;
    }
    int n = mbedtls_ssl_read(&tls->ssl, reinterpret_cast<unsigned char *>(out), len);
    if (n > 0) return n;
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
        n == MBEDTLS_ERR_SSL_TIMEOUT) {
      return 0;
    }
    return -1;
  }
};

void fail(Reply &r, Error error, const char *detail) {
  r.error = error;
  strlcpy(r.detail, detail, sizeof(r.detail));
}

bool handshake(Conn &c, const String &host, Reply &r) {
  Tls &t = *c.tls;
  t.net.fd = c.tcp.fd();
  bool ready =
      mbedtls_ctr_drbg_seed(&t.drbg, mbedtls_entropy_func, &t.entropy, nullptr, 0) == 0 &&
      mbedtls_ssl_config_defaults(&t.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                  MBEDTLS_SSL_PRESET_DEFAULT) == 0;
  if (ready) {
    mbedtls_ssl_conf_authmode(&t.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&t.conf, &t.noRoots, nullptr);
    mbedtls_ssl_conf_verify(&t.conf, verify, nullptr);
    mbedtls_ssl_conf_rng(&t.conf, mbedtls_ctr_drbg_random, &t.drbg);
    mbedtls_ssl_conf_read_timeout(&t.conf, 250);
    ready = mbedtls_ssl_setup(&t.ssl, &t.conf) == 0 &&
            mbedtls_ssl_set_hostname(&t.ssl, host.c_str()) == 0;
  }
  if (!ready) {
    fail(r, Error::NO_RESPONSE, "not enough memory");
    return false;
  }
  mbedtls_ssl_set_bio(&t.ssl, &t.net, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);

  int ret;
  while ((ret = mbedtls_ssl_handshake(&t.ssl)) != 0) {
    bool again = ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
                 ret == MBEDTLS_ERR_SSL_TIMEOUT;
    if (!again || c.expired()) break;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  if (ret == 0) {
    const mbedtls_x509_crt *crt = mbedtls_ssl_get_peer_cert(&t.ssl);
    if (crt) r.certExpiry = epochOf(crt->valid_to);
    return true;
  }
  uint32_t flags = mbedtls_ssl_get_verify_result(&t.ssl);
  if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags != 0xFFFFFFFFu) {
    if (flags & MBEDTLS_X509_BADCERT_EXPIRED) fail(r, Error::CERTIFICATE, "certificate expired");
    else if (flags & MBEDTLS_X509_BADCERT_FUTURE)
      fail(r, Error::CERTIFICATE, "certificate not yet valid");
    else if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
      fail(r, Error::CERTIFICATE, "certificate name mismatch");
    else if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
      fail(r, Error::CERTIFICATE, "certificate not trusted");
    else fail(r, Error::CERTIFICATE, "certificate not accepted");
  } else if (c.expired()) {
    fail(r, Error::TIMEOUT, "timeout");
  } else {
    fail(r, Error::NO_RESPONSE, "secure connection failed");
  }
  return false;
}

// `connectMs` is how long the TCP connection took; the reply time is added
// to it. The TLS handshake in between is left out: on this chip it is
// seconds of arithmetic that say nothing about the server.
void exchange(Conn &c, const Url &url, const char *method, const String &headers,
              const char *body, uint32_t connectMs, Reply &r) {
  uint32_t startedMs = millis();
  bool defaultPort = url.port == (url.https ? 443 : 80);
  String request = String(method) + " " + url.path + " HTTP/1.1\r\nHost: " + url.host;
  if (!defaultPort) request += ":" + String(url.port);
  request += "\r\nUser-Agent: UptimeSentinel/";
  request += cfg::VERSION;
  request += "\r\nAccept: */*\r\nConnection: close\r\n";
  request += headers;
  if (body) request += "Content-Length: " + String(strlen(body)) + "\r\n";
  request += "\r\n";
  if (!c.write(request.c_str(), request.length()) || (body && !c.write(body, strlen(body)))) {
    fail(r, c.expired() ? Error::TIMEOUT : Error::NO_RESPONSE,
         c.expired() ? "timeout" : "connection lost");
    return;
  }

  char line[40];
  size_t len = 0;
  bool done = false, closed = false;
  while (!done && !closed && !c.expired()) {
    char buf[48];
    int n = c.read(buf, sizeof(buf));
    if (n < 0) closed = true;
    else if (n == 0) vTaskDelay(pdMS_TO_TICKS(10));
    for (int i = 0; i < n && !done; i++) {
      if (buf[i] == '\n') done = true;
      else if (len < sizeof(line) - 1) line[len++] = buf[i];
    }
  }
  line[len] = 0;
  r.ms = static_cast<int32_t>(connectMs + (millis() - startedMs));
  if (!done) {
    if (closed) fail(r, len ? Error::BAD_REPLY : Error::NO_RESPONSE, len ? "bad reply" : "no reply");
    else fail(r, Error::TIMEOUT, "timeout");
    return;
  }
  int code = 0;
  if (strncmp(line, "HTTP/", 5) == 0) {
    const char *space = strchr(line, ' ');
    if (space) code = atoi(space + 1);
  }
  if (code < 100 || code > 599) fail(r, Error::BAD_REPLY, "bad reply");
  else r.code = static_cast<int16_t>(code);
}

void run(const Url &url, const char *method, const String &headers, const char *body,
         uint32_t timeoutMs, Reply &r) {
  Conn c;
  uint32_t started = millis();
  if (!c.tcp.connect(url.host.c_str(), url.port, timeoutMs)) {
    fail(r, Error::NO_RESPONSE, "no response");
    return;
  }
  // The whole exchange gets two timeouts' worth: one to connect and
  // handshake, one for the server to answer.
  uint32_t connectMs = millis() - started;
  c.deadline = started + 2 * timeoutMs;
  if (url.https) {
    c.tls = new (std::nothrow) Tls();
    if (!c.tls) fail(r, Error::NO_RESPONSE, "not enough memory");
    else if (handshake(c, url.host, r)) exchange(c, url, method, headers, body, connectMs, r);
    if (c.tls) {
      if (r.error == Error::NONE) mbedtls_ssl_close_notify(&c.tls->ssl);
      delete c.tls;
    }
  } else {
    exchange(c, url, method, headers, body, connectMs, r);
  }
  c.tcp.stop();
}

}  // namespace

Reply request(const String &address, const char *method, const String &headers, const char *body,
              uint32_t timeoutMs) {
  Reply r = {Error::NONE, 0, -1, 0, ""};
  Url url;
  if (!parseUrl(address, url)) {
    fail(r, Error::BAD_ADDRESS, "bad address");
    return r;
  }
  if (url.https) {
    net::TlsLock one;
    run(url, method, headers, body, timeoutMs, r);
  } else {
    run(url, method, headers, body, timeoutMs, r);
  }
  return r;
}

}  // namespace fetch
