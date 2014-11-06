/*
  Copyright 2014 DataStax

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "ssl.hpp"

#include "common.hpp"

#include "third_party/boost/boost/utility/string_ref.hpp"
#include "third_party/boost/boost/algorithm/string.hpp"
#include "third_party/rb/ring_buffer_bio.hpp"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <string.h>

#define DEBUG_SSL 0

namespace cass {

static int ssl_no_verify_callback(int ok, X509_STORE_CTX* store) {
  // Verification happens after in SslSession::verify() via
  // SSL_get_verify_result().
  return 1;
}

#define SSL_PRINT_INFO(ssl, w, flag, msg) do { \
    if(w & flag) {                             \
      fprintf(stderr, "%s - %s - %s\n",        \
              msg,                             \
              SSL_state_string(ssl),           \
              SSL_state_string_long(ssl));     \
    }                                          \
 } while(0);

void ssl_info_callback(const SSL* ssl, int where, int ret) {
  if(ret == 0) {
    fprintf(stderr, "ssl_info_callback, error occured.\n");
    return;
  }
  SSL_PRINT_INFO(ssl, where, SSL_CB_LOOP, "LOOP");
  SSL_PRINT_INFO(ssl, where, SSL_CB_EXIT, "EXIT");
  SSL_PRINT_INFO(ssl, where, SSL_CB_READ, "READ");
  SSL_PRINT_INFO(ssl, where, SSL_CB_WRITE, "WRITE");
  SSL_PRINT_INFO(ssl, where, SSL_CB_ALERT, "ALERT");
  SSL_PRINT_INFO(ssl, where, SSL_CB_HANDSHAKE_DONE, "HANDSHAKE DONE");
}

#undef SSL_PRINT_INFO

static int pem_password_callback(char* buf, int size, int rwflag, void* u) {
  if (u != NULL) {
    size_t to_copy = size;
    size_t len = strlen(static_cast<const char*>(u));
    if (len < to_copy) {
      to_copy = len;
    }
    memcpy(buf, u, to_copy);
  }
  return 0;
}

static uv_rwlock_t* crypto_locks;

static void crypto_locking_callback(int mode, int n, const char* file, int line) {
  if (mode & CRYPTO_LOCK) {
    if (mode & CRYPTO_READ) {
      uv_rwlock_rdlock(crypto_locks + n);
    } else {
      uv_rwlock_wrlock(crypto_locks + n);
    }
  } else {
    if (mode & CRYPTO_READ) {
      uv_rwlock_rdunlock(crypto_locks + n);
    } else {
      uv_rwlock_wrunlock(crypto_locks + n);
    }
  }
}

static unsigned long crypto_id_callback() {
  return uv_thread_self();
}

static X509* load_cert(const char* cert, size_t cert_size) {
  BIO* bio = BIO_new_mem_buf(const_cast<char*>(cert), cert_size);
  if (bio == NULL) {
    return NULL;
  }

  X509* x509 = PEM_read_bio_X509(bio, NULL, pem_password_callback, NULL);
  if (x509 == NULL) {
    // TODO: Replace with global logging
    ERR_print_errors_fp(stderr);
  }

  BIO_free_all(bio);

  return x509;
}

// Implementation taken from OpenSSL's SSL_CTX_use_certificate_chain_file()
// (https://github.com/openssl/openssl/blob/OpenSSL_0_9_8-stable/ssl/ssl_rsa.c#L705).
// Modified to be used for in-memory certficate chains and formatting.
static int SSL_CTX_use_certificate_chain_bio(SSL_CTX* ctx, BIO* in) {
  int ret = 0;
  X509* x = NULL;

  x = PEM_read_bio_X509_AUX(in, NULL, pem_password_callback, NULL);
  if (x == NULL) {
    SSLerr(SSL_F_SSL_CTX_USE_CERTIFICATE_CHAIN_FILE,ERR_R_PEM_LIB);
    goto end;
  }

  ret = SSL_CTX_use_certificate(ctx, x);

  if (ERR_peek_error() != 0) {
    // Key/certificate mismatch doesn't imply ret==0 ...
    ret = 0;
  }

  if (ret) {
    // If we could set up our certificate, now proceed to
    // the CA certificates.

    X509 *ca;
    int r;
    unsigned long err;

    if (ctx->extra_certs != NULL) {
      sk_X509_pop_free(ctx->extra_certs, X509_free);
      ctx->extra_certs = NULL;
    }

    while ((ca = PEM_read_bio_X509(in, NULL, pem_password_callback, NULL)) != NULL) {
      r = SSL_CTX_add_extra_chain_cert(ctx, ca);
      if (!r) {
        X509_free(ca);
        ret = 0;
        goto end;
      }
      // Note that we must not free r if it was successfully
      // added to the chain (while we must free the main
      // certificate, since its reference count is increased
      // by SSL_CTX_use_certificate).
    }
    // When the while loop ends, it's usually just EOF.
    err = ERR_peek_last_error();
    if (ERR_GET_LIB(err) == ERR_LIB_PEM && ERR_GET_REASON(err) == PEM_R_NO_START_LINE) {
      ERR_clear_error();
    } else {
      // Some real error
      ret = 0;
    }
  }

end:
  if (x != NULL) X509_free(x);
  return ret;
}

static EVP_PKEY* load_key(const char* key,
                          size_t key_size,
                          const char* password) {
  BIO* bio = BIO_new_mem_buf(const_cast<char*>(key), key_size);
  if (bio == NULL) {
    return NULL;
  }

  EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio,
                                           NULL,
                                           pem_password_callback,
                                           const_cast<char*>(password));
  if (pkey == NULL) {
    // TODO: Replace with global logging
    ERR_print_errors_fp(stderr);
  }

  BIO_free_all(bio);

  return pkey;
}

class OpenSslVerifyIdentity {
public:
  enum Result {
    INVALID_CERT,
    MATCH,
    NO_MATCH,
    NO_SAN_PRESENT
  };

  static Result match(X509* cert, const boost::string_ref& to_match, int type) {
    assert(type == GEN_IPADD);
    return match_subject_alt_names(cert, to_match, type);
  }

private:
  static Result match_subject_alt_names(X509* cert, const boost::string_ref& to_match, int type) {
    STACK_OF(GENERAL_NAME)* names
      = static_cast<STACK_OF(GENERAL_NAME)*>(X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL));
    if (names == NULL) {
      return NO_SAN_PRESENT;
    }

    for (int i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
      GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);

      if (name->type != type) continue;

      if (type == GEN_DNS) {
        ASN1_STRING* str = name->d.dNSName;
        boost::string_ref dsn_name(copy_cast<unsigned char*, char*>(ASN1_STRING_data(str)), ASN1_STRING_length(str));
        if (boost::iequals(dsn_name, to_match)) {
          return MATCH;
        }
      } else {
        ASN1_STRING* str = name->d.iPAddress;
        unsigned char* ip = ASN1_STRING_data(str);
        int ip_len = ASN1_STRING_length(str);
        if (ip_len != 4 || ip_len != 16) {
          return INVALID_CERT;
        }
        if (static_cast<size_t>(ip_len) == to_match.length() &&
            memcmp(ip, to_match.data(), to_match.length()) == 0) {
          return MATCH;
        }
      }
    }
    sk_GENERAL_NAME_pop_free(names, GENERAL_NAME_free);

    return NO_MATCH;
  }

};

OpenSslSession::OpenSslSession(const Address& address,
                               int flags,
                               SSL_CTX* ssl_ctx)
  : SslSession(address, flags)
  , ssl_(SSL_new(ssl_ctx))
  , incoming_bio_(rb::RingBufferBio::create(&incoming_))
  , outgoing_bio_(rb::RingBufferBio::create(&outgoing_)) {
  SSL_set_bio(ssl_, incoming_bio_, outgoing_bio_);
  SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, ssl_no_verify_callback);
#if DEBUG_SSL
  SSL_CTX_set_info_callback(ssl_ctx, ssl_info_callback);
#endif
  SSL_set_connect_state(ssl_);
}

OpenSslSession::~OpenSslSession() {
  SSL_free(ssl_);
}

void OpenSslSession::do_handshake() {
  int rc = SSL_connect(ssl_);
  if (rc <= 0) {
    check_error(rc);
  }
}

void OpenSslSession::verify() {
  if (verify_flags_ & CASS_SSL_VERIFY_NONE) return;

  X509* peer_cert = SSL_get_peer_certificate(ssl_);
  if (peer_cert == NULL) {
    error_code_ = CASS_ERROR_SSL_NO_PEER_CERT;
    error_message_ = "No peer certificate found";
    return;
  }

  if (verify_flags_ & CASS_SSL_VERIFY_PEER_CERT) {
    int rc = SSL_get_verify_result(ssl_);
    if (rc != X509_V_OK) {
      error_code_ = CASS_ERROR_SSL_INVALID_PEER_CERT;
      error_message_ = X509_verify_cert_error_string(rc);
      X509_free(peer_cert);
      return;
    }
  }

  if (verify_flags_ & CASS_SSL_VERIFY_PEER_IDENTITY) {
    // We can only match IP address because that's what
    // Cassandra has in system local/peers tables
    char buf[16];
    size_t buf_size;
    if (addr_.family() == AF_INET) {
      buf_size = 4;
      memcpy(buf, &addr_.addr_in()->sin_addr.s_addr, buf_size);
    } else {
      buf_size = 16;
      memcpy(buf, &addr_.addr_in6()->sin6_addr, buf_size);
    }
    OpenSslVerifyIdentity::Result result
        = OpenSslVerifyIdentity::match(peer_cert,
                                       boost::string_ref(buf, buf_size),
                                       GEN_IPADD);

    if (result == OpenSslVerifyIdentity::INVALID_CERT) {
      error_code_ = CASS_ERROR_SSL_INVALID_PEER_CERT;
      error_message_ = "Peer certificate has malformed subject name(s)";
      X509_free(peer_cert);
      return;
    } else if (result != OpenSslVerifyIdentity::MATCH) {
      error_code_ = CASS_ERROR_SSL_IDENTITY_MISMATCH;
      error_message_ = "Peer certificate subject name does not match";
      X509_free(peer_cert);
      return;
    }
  }

  X509_free(peer_cert);
}

int OpenSslSession::encrypt(const char* buf, size_t size) {
  int rc = SSL_write(ssl_, buf, size);
  if (rc <= 0) {
    check_error(rc);
  }
  return rc;
}

int OpenSslSession::decrypt(char* buf, size_t size)  {
  int rc = SSL_read(ssl_, buf, size);
  if (rc <= 0) {
    check_error(rc);
  }
  return rc;
}

bool OpenSslSession::check_error(int rc) {
  int err = SSL_get_error(ssl_, rc);
  if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_NONE) {
    char buf[128];
    ERR_error_string_n(err, buf, sizeof(buf));
    error_message_ = buf;
    return true;
  }
  return false;
}

OpenSslContext::OpenSslContext()
  : ssl_ctx_(SSL_CTX_new(SSLv23_client_method()))
  , trusted_store_(X509_STORE_new()) {
  SSL_CTX_set_cert_store(ssl_ctx_, trusted_store_);
}

OpenSslContext::~OpenSslContext() {
  SSL_CTX_free(ssl_ctx_);
}

SslSession* OpenSslContext::create_session(const Address& address ) {
  return new OpenSslSession(address, verify_flags_, ssl_ctx_);
}

CassError OpenSslContext::add_trusted_cert(CassString cert) {
  X509* x509 = load_cert(cert.data, cert.length);
  if (x509 == NULL) {
    return CASS_ERROR_SSL_INVALID_CERT;
  }

  X509_STORE_add_cert(trusted_store_, x509);
  X509_free(x509);

  return CASS_OK;
}

CassError OpenSslContext::set_cert(CassString cert) {
  BIO* bio = BIO_new_mem_buf(const_cast<char*>(cert.data), cert.length);
  if (bio == NULL) {
    return CASS_ERROR_SSL_INVALID_CERT;
  }

  int rc = SSL_CTX_use_certificate_chain_bio(ssl_ctx_, bio);

  BIO_free_all(bio);

  if (!rc) {
    // TODO: Replace with global logging
    ERR_print_errors_fp(stderr);
    return CASS_ERROR_SSL_INVALID_CERT;
  }

  return CASS_OK;
}

CassError OpenSslContext::set_private_key(CassString key, const char* password) {
  EVP_PKEY* pkey = load_key(key.data, key.length, password);
  if (pkey == NULL) {
    return CASS_ERROR_SSL_INVALID_PRIVATE_KEY;
  }

  SSL_CTX_use_PrivateKey(ssl_ctx_, pkey);
  EVP_PKEY_free(pkey);

  return CASS_OK;
}

SslContext* OpenSslContextFactory::create() {
  return new OpenSslContext();
}

void OpenSslContextFactory::init() {
  SSL_library_init();
  SSL_load_error_strings();
  OpenSSL_add_all_algorithms();

  // We have to set the lock/id callbacks for use of OpenSSL thread safety.
  // It's not clear what's thread-safe in OpenSSL. Writing/Reading to
  // a single "SSL" object is NOT and we don't do that, but we do create multiple
  // "SSL" objects from a single "SSL_CTX" in different threads. That seems to be
  // okay with the following callbacks set.
  int num_locks = CRYPTO_num_locks();
  crypto_locks = new uv_rwlock_t[num_locks];
  for (int i = 0; i < num_locks; ++i) {
    if (uv_rwlock_init(crypto_locks + i)) {
      fprintf(stderr, "Unable to init read/write lock");
      abort();
    }
  }

  CRYPTO_set_locking_callback(crypto_locking_callback);
  CRYPTO_set_id_callback(crypto_id_callback);
}


} // namespace cass