// One HTTP or HTTPS request to an address the user typed in: sends it,
// reads the status line and stops. Used by the website checks and by the
// alert channels that have a user-supplied address.
//
// HTTPS servers are verified against the CA bundle (ca_bundle.h) with our
// own check instead of the framework's, because that one rejects a chain in
// which the server also sends its root with an old cross-signature, as many
// large hosting providers do. Here a chain is trusted when one of its
// certificates is signed by a root in the bundle, or is such a root itself.
#pragma once
#include <Arduino.h>

namespace fetch {

enum class Error : uint8_t { NONE, BAD_ADDRESS, NO_RESPONSE, CERTIFICATE, TIMEOUT, BAD_REPLY };

struct Reply {
  Error error;          // NONE when a status line arrived, whatever the status
  int16_t code;         // HTTP status, 0 = none
  int32_t ms;           // time to connect plus time to answer (TLS handshake left out), -1 = none
  uint32_t certExpiry;  // unix time the server certificate expires, 0 = not HTTPS or unknown
  char detail[28];      // a few words for the user when error is not NONE
};

// `headers` is zero or more complete "Name: value\r\n" lines. With a null
// `body` no body is sent. Holds net::TlsLock for the length of an HTTPS call.
Reply request(const String &url, const char *method, const String &headers, const char *body,
              uint32_t timeoutMs);

}  // namespace fetch
