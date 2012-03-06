/*
 *  Copyright 2012 Rackspace
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

#include "luv.h"
#include "luv_tls.h"

#include <assert.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

/**
 * This module is hevily inspired by Node.js' node_crypto.cc:
 *   <https://github.com/joyent/node/blob/master/src/node_crypto.cc>
 */

/**
 * We hard code a check here for the version of OpenSSL we bundle inside deps, because its
 * too easily to accidently pull in an older version of OpenSSL on random platforms with
 * weird include paths.
 */
#if OPENSSL_VERSION_NUMBER != LUVIT_OPENSSL_VERSION_NUMBER
#error Invalid OpenSSL version number. Busted Include Paths?
#endif

/* TLS object that maps to an individual connection */
typedef struct tls_conn_t {
  BIO *bio_read;
  BIO *bio_write;
  SSL *ssl;
  int is_server;
  int error;
} tls_conn_t;

static const int X509_NAME_FLAGS = ASN1_STRFLGS_ESC_CTRL
  | ASN1_STRFLGS_ESC_MSB
  | XN_FLAG_SEP_MULTILINE
  | XN_FLAG_FN_SN;

#define TLS_CONNECTION_HANDLE "ltls_connection"

#ifdef SSL_DEBUG
#define DBG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG(...)
#endif

/**
 * TLS Connection Methods
 */

static int
tls_conn_verify_cb(int preverify_ok, X509_STORE_CTX *ctx) {
  /*
   * Since we cannot perform I/O quickly enough in this callback, we ignore
   * all preverify_ok errors and let the handshake continue. It is
   * imparative that the user use connection:verifyError after the
   * 'secure' callback has been made.
   */
  return 1;
}

/**
  * Lua constructor
  *  (SecureContext, [isServer(boolean), isRequestCert(boolean), isRejectUnauthorized(boolean)]
  */
static tls_conn_t*
newCONN(lua_State *L)
{
  tls_sc_t* sc = luvit__lua_tls_sc_get(L, 1);
  int is_server = lua_toboolean(L, 2);
  int is_request_cert = lua_toboolean(L, 3);
  int is_reject_unauthorized = lua_toboolean(L, 4);
  tls_conn_t* tc;
  int verify_mode;

  tc = lua_newuserdata(L, sizeof(tls_conn_t));
  tc->bio_read = BIO_new(BIO_s_mem());
  tc->bio_write = BIO_new(BIO_s_mem());
  tc->ssl = SSL_new(sc->ctx);
  tc->is_server = is_server;
  tc->error = 0;

  if (tc->is_server) {
    if (!is_request_cert) {
      verify_mode = SSL_VERIFY_NONE;
    }
    else {
      verify_mode = SSL_VERIFY_PEER;
      if (is_reject_unauthorized) {
        verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
      }
    }
  }
  else {
    verify_mode = SSL_VERIFY_NONE;
  }

  SSL_set_app_data(tc->ssl, tc);
  SSL_set_bio(tc->ssl, tc->bio_read, tc->bio_write);
  SSL_set_mode(tc->ssl, SSL_get_mode(tc->ssl) | SSL_MODE_RELEASE_BUFFERS);
  /* Always allow a connection. We'll reject in lua. */
  SSL_set_verify(tc->ssl, verify_mode, tls_conn_verify_cb);

  tc->is_server ? SSL_set_accept_state(tc->ssl) : SSL_set_connect_state(tc->ssl);

  luaL_getmetatable(L, TLS_CONNECTION_HANDLE);
  lua_setmetatable(L, -2);
  return tc;
}

static tls_conn_t*
getCONN(lua_State *L, int index)
{
  tls_conn_t *tc = luaL_checkudata(L, index, TLS_CONNECTION_HANDLE);
  return tc;
}

int
luvit__lua_tls_conn_create(lua_State *L) {
  (void) newCONN(L);
  return 1;
}

static int
tls_handle_ssl_error_x(SSL *ssl, int rv, const char *func) {
  if (rv >= 0) {
    return rv;
  }

  int err = SSL_get_error(ssl, rv);

  if (err == SSL_ERROR_NONE) {
    return 0;
  }
  else if (err == SSL_ERROR_WANT_WRITE) {
    DBG("[%p] SSL: %s want write\n", ssl, func);
    return 0;
  }
  else if (err == SSL_ERROR_WANT_READ) {
    DBG("[%p] SSL: %s want read\n", ssl, func);
    return 0;
  }
  else {
    BIO *bio;
    BUF_MEM *mem;
    assert(err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL);
    if ((bio = BIO_new(BIO_s_mem()))) {
      ERR_print_errors(bio);
      BIO_get_mem_ptr(bio, &mem);
      BIO_free(bio);
    }
    return rv;
  }

  return 0;
}

static int
tls_handle_bio_error_x(tls_conn_t *tc, BIO *bio, SSL *ssl, int rv, const char *func) {
  int retry;

  if (rv >= 0) {
    return rv;
  }

  retry = BIO_should_retry(bio);

  if (BIO_should_write(bio)) {
    DBG("[%p] BIO: %s want write. should retry %d\n", ssl, func, retry);
    return 0;

  } else if (BIO_should_read(bio)) {
    DBG("[%p] BIO: %s want read. should retry %d\n", ssl, func, retry);
    return 0;

  } else {
    char ssl_error_buf[512];
    assert(rv == SSL_ERROR_SSL || rv == SSL_ERROR_SYSCALL);
    tc->error = rv;
    ERR_error_string_n(rv, ssl_error_buf, sizeof(ssl_error_buf));
    DBG("[%p] BIO: %s failed: (%d) %s\n", ssl, func, rv, ssl_error_buf);
    return rv;
  }

  return 0;
}

#define tls_handle_ssl_error(ssl, rv) tls_handle_ssl_error_x(ssl, rv, __FUNCTION__)
#define tls_handle_bio_error(tc, bio, ssl, rv) tls_handle_bio_error_x(tc, bio, ssl, rv, __FUNCTION__)

static int
tls_conn_start(lua_State *L) {
  tls_conn_t *tc = getCONN(L, 1);
  int rv = 0;

  if (!SSL_is_init_finished(tc->ssl)) {
    if (tc->is_server) {
      rv = SSL_accept(tc->ssl);
    }
    else {
      rv = SSL_connect(tc->ssl);
    }
    tls_handle_ssl_error(tc->ssl, rv);
  }
  lua_pushnumber(L, rv);
  return 1;
}

static int
tls_conn_close(lua_State *L) {
  tls_conn_t *tc = getCONN(L, 1);
  SSL_free(tc->ssl);
  tc->ssl = NULL;
  return 0;
}

static int
tls_conn_enc_in(lua_State *L) {
  size_t len;
  tls_conn_t *tc = getCONN(L, 1);
  const char *data = luaL_checklstring(L, 2, &len);
  int bytes_written = BIO_write(tc->bio_read, data, len);
  tls_handle_bio_error(tc, tc->bio_read, tc->ssl, bytes_written);
  lua_pushnumber(L, bytes_written);
  return 1;
}

static int
tls_conn_enc_out(lua_State *L) {
  char pool[4096];
  tls_conn_t *tc = getCONN(L, 1);
  int bytes_read = BIO_read(tc->bio_write, pool, sizeof(pool));
  tls_handle_bio_error(tc, tc->bio_write, tc->ssl, bytes_read);
  lua_pushnumber(L, bytes_read);
  if (bytes_read > 0) {
    lua_pushlstring(L, pool, bytes_read);
  }
  else {
    lua_pushlstring(L, "", 0);
  }
  return 2;
}

static int
tls_conn_enc_pending(lua_State *L) {
  tls_conn_t *tc = getCONN(L, 1);
  int bytes_pending = BIO_pending(tc->bio_write);
  lua_pushnumber(L, bytes_pending);
  return 1;
}

static int
tls_conn_clear_out(lua_State *L) {
  tls_conn_t *tc = getCONN(L, 1);
  char pool[4096];
  int bytes_read;

  if (!SSL_is_init_finished(tc->ssl)) {
    int rv;

    if (tc->is_server) {
      rv = SSL_accept(tc->ssl);
      tls_handle_ssl_error(tc->ssl, rv);
    } else {
      rv = SSL_connect(tc->ssl);
      tls_handle_ssl_error(tc->ssl, rv);
    }

    if (rv < 0) {
      lua_pushnumber(L, rv);
      return 1;
    }
  }

  bytes_read = SSL_read(tc->ssl, pool, sizeof(pool));
  tls_handle_ssl_error(tc->ssl, bytes_read);
  lua_pushnumber(L, bytes_read);
  if (bytes_read > 0) {
    lua_pushlstring(L, pool, bytes_read);
  }
  else {
    lua_pushlstring(L, "", 0);
  }
  return 2;
}

static int
tls_conn_clear_in(lua_State *L) {
  size_t len;
  tls_conn_t *tc = getCONN(L, 1);
  const char *data = luaL_checklstring(L, 2, &len);
  int bytes_written;

  if (!SSL_is_init_finished(tc->ssl)) {
    int rv;
    if (tc->is_server) {
      rv = SSL_accept(tc->ssl);
      tls_handle_ssl_error(tc->ssl, rv);
    } else {
      rv = SSL_connect(tc->ssl);
      tls_handle_ssl_error(tc->ssl, rv);
    }

    if (rv < 0) {
      lua_pushnumber(L, rv);
      return 1;
    }
  }

  bytes_written = SSL_write(tc->ssl, data, len);
  DBG("bytes_written = %d, len = %d\n", bytes_written, len);
  tls_handle_ssl_error(tc->ssl, bytes_written);
  lua_pushnumber(L, bytes_written);
  return 1;
}

static int
tls_conn_clear_pending(lua_State *L) {
  tls_conn_t *tc = getCONN(L, 1);
  int bytes_pending = BIO_pending(tc->bio_read);
  lua_pushnumber(L, bytes_pending);
  return 1;
}

static int
tls_conn_shutdown(lua_State *L) {
  tls_conn_t *tc = getCONN(L, 1);
  int rv = SSL_shutdown(tc->ssl);
  lua_pushnumber(L, rv);
  return 1;
}

static int
tls_conn_get_error(lua_State *L) {
  tls_conn_t *tc = getCONN(L, 1);
  tc->error ? lua_pushnumber(L, tc->error) : lua_pushnil(L);
  return 1;
}

static int
tls_conn_clear_error(lua_State *L) {
  tls_conn_t *tc = getCONN(L, 1);
  tc->error = 0;
  return 0;
}

static int
tls_conn_verify_error(lua_State *L) {
  tls_conn_t *tc = getCONN(L, 1);
  X509* peer_cert = SSL_get_peer_certificate(tc->ssl);
  if (!peer_cert) {
    lua_pushstring(L, "Unable to get peer certificate");
  }
  else {
    long verify = SSL_get_verify_result(tc->ssl);
    switch (verify) {
    case X509_V_OK:
      lua_pushnil(L);
      break;
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
      lua_pushstring(L, "UNABLE_TO_GET_ISSUER_CERT");
      break;
    case X509_V_ERR_UNABLE_TO_GET_CRL:
      lua_pushstring(L, "UNABLE_TO_GET_CRL");
      break;
    case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE:
      lua_pushstring(L, "UNABLE_TO_DECRYPT_CERT_SIGNATURE");
      break;
    case X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE:
      lua_pushstring(L, "UNABLE_TO_DECRYPT_CRL_SIGNATURE");
      break;
    case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY:
      lua_pushstring(L, "UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY");
      break;
    case X509_V_ERR_CERT_SIGNATURE_FAILURE:
      lua_pushstring(L, "CERT_SIGNATURE_FAILURE");
      break;
    case X509_V_ERR_CRL_SIGNATURE_FAILURE:
      lua_pushstring(L, "CRL_SIGNATURE_FAILURE");
      break;
    case X509_V_ERR_CERT_NOT_YET_VALID:
      lua_pushstring(L, "CERT_NOT_YET_VALID");
      break;
    case X509_V_ERR_CERT_HAS_EXPIRED:
      lua_pushstring(L, "CERT_HAS_EXPIRED");
      break;
    case X509_V_ERR_CRL_NOT_YET_VALID:
      lua_pushstring(L, "CRL_NOT_YET_VALID");
      break;
    case X509_V_ERR_CRL_HAS_EXPIRED:
      lua_pushstring(L, "CRL_HAS_EXPIRED");
      break;
    case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
      lua_pushstring(L, "ERROR_IN_CERT_NOT_BEFORE_FIELD");
      break;
    case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
      lua_pushstring(L, "ERROR_IN_CERT_NOT_AFTER_FIELD");
      break;
    case X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD:
      lua_pushstring(L, "ERROR_IN_CRL_LAST_UPDATE_FIELD");
      break;
    case X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD:
      lua_pushstring(L, "ERROR_IN_CRL_NEXT_UPDATE_FIELD");
      break;
    case X509_V_ERR_OUT_OF_MEM:
      lua_pushstring(L, "OUT_OF_MEM");
      break;
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
      lua_pushstring(L, "DEPTH_ZERO_SELF_SIGNED_CERT");
      break;
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
      lua_pushstring(L, "SELF_SIGNED_CERT_IN_CHAIN");
      break;
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
      lua_pushstring(L, "UNABLE_TO_GET_ISSUER_CERT_LOCALLY");
      break;
    case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
      lua_pushstring(L, "UNABLE_TO_VERIFY_LEAF_SIGNATURE");
      break;
    case X509_V_ERR_CERT_CHAIN_TOO_LONG:
      lua_pushstring(L, "CERT_CHAIN_TOO_LONG");
      break;
    case X509_V_ERR_CERT_REVOKED:
      lua_pushstring(L, "CERT_REVOKED");
      break;
    case X509_V_ERR_INVALID_CA:
      lua_pushstring(L, "INVALID_CA");
      break;
    case X509_V_ERR_PATH_LENGTH_EXCEEDED:
      lua_pushstring(L, "PATH_LENGTH_EXCEEDED");
      break;
    case X509_V_ERR_INVALID_PURPOSE:
      lua_pushstring(L, "INVALID_PURPOSE");
      break;
    case X509_V_ERR_CERT_UNTRUSTED:
      lua_pushstring(L, "CERT_UNTRUSTED");
      break;
    case X509_V_ERR_CERT_REJECTED:
      lua_pushstring(L, "CERT_REJECTED");
      break;
    default:
      lua_pushstring(L, X509_verify_cert_error_string(verify));
      break;
    }
    X509_free(peer_cert);
  }
  return 1;
}

static int
tls_conn_is_init_finished(lua_State *L) {
  tls_conn_t *tc = getCONN(L, 1);
  lua_pushboolean(L, SSL_is_init_finished(tc->ssl));
  return 1;
}

static int
tls_conn_get_current_cipher(lua_State *L) {
  SSL_CIPHER *c;
  tls_conn_t *tc = getCONN(L, 1);
  if (!tc->ssl) {
    lua_pushnil(L);
    return 1;
  }
  c = SSL_get_current_cipher(tc->ssl);
  if (c == NULL) {
    lua_pushnil(L);
    return 1;
  }
  lua_newtable(L);
  lua_pushstring(L, "name");
  lua_pushstring(L, SSL_CIPHER_get_name(c));
  lua_settable(L, -3);
  lua_pushstring(L, "version");
  lua_pushstring(L, SSL_CIPHER_get_version(c));
  lua_settable(L, -3);
  return 1;
}

static int
tls_conn_get_peer_certificate(lua_State *L)
{
  X509 *peer_cert = NULL;
  BIO *bio = NULL;
  BUF_MEM* mem;
  EVP_PKEY *pkey = NULL;
  RSA *rsa = NULL;
  tls_conn_t *tc = NULL;
  int index;
  unsigned int md_size, i;
  unsigned char md[EVP_MAX_MD_SIZE];
  STACK_OF(ASN1_OBJECT) *eku;

  tc = getCONN(L, 1);
  if (!tc->ssl) {
    lua_pushnil(L);
    return 1;
  }
  peer_cert = SSL_get_peer_certificate(tc->ssl);
  if (!peer_cert) {
    lua_pushnil(L);
    return 1;
  }

  bio = BIO_new(BIO_s_mem());
  lua_newtable(L);

  if (X509_NAME_print_ex(bio, X509_get_subject_name(peer_cert), 0,
                         X509_NAME_FLAGS) > 0) {
    BIO_get_mem_ptr(bio, &mem);
    lua_pushstring(L, "subject");
    lua_pushlstring(L, mem->data, mem->length);
    lua_settable(L, -3);
  }
  (void) BIO_reset(bio);

  if (X509_NAME_print_ex(bio, X509_get_issuer_name(peer_cert), 0,
                         X509_NAME_FLAGS) > 0) {
    BIO_get_mem_ptr(bio, &mem);
    lua_pushstring(L, "issuer");
    lua_pushlstring(L, mem->data, mem->length);
    lua_settable(L, -3);
  }
  (void) BIO_reset(bio);

  index = X509_get_ext_by_NID(peer_cert, NID_subject_alt_name, -1);
  if (index >= 0) {
    X509_EXTENSION* ext;
    int rv;

    ext = X509_get_ext(peer_cert, index);
    assert(ext != NULL);

    rv = X509V3_EXT_print(bio, ext, 0, 0);
    assert(rv == 1);

    BIO_get_mem_ptr(bio, &mem);
    lua_pushstring(L, "subjectaltname");
    lua_pushlstring(L, mem->data, mem->length);
    lua_settable(L, -3);

    (void) BIO_reset(bio);
  }

  if( NULL != (pkey = X509_get_pubkey(peer_cert))
      && NULL != (rsa = EVP_PKEY_get1_RSA(pkey)) ) {
    BN_print(bio, rsa->n);
    BIO_get_mem_ptr(bio, &mem);
    lua_pushstring(L, "modulus");
    lua_pushlstring(L, mem->data, mem->length);
    lua_settable(L, -3);
    (void) BIO_reset(bio);

    BN_print(bio, rsa->e);
    BIO_get_mem_ptr(bio, &mem);
    lua_pushstring(L, "e");
    lua_pushlstring(L, mem->data, mem->length);
    lua_settable(L, -3);
    (void) BIO_reset(bio);
  }

  ASN1_TIME_print(bio, X509_get_notBefore(peer_cert));
  BIO_get_mem_ptr(bio, &mem);
  lua_pushstring(L, "valid_from");
  lua_pushlstring(L, mem->data, mem->length);
  lua_settable(L, -3);
  (void) BIO_reset(bio);

  ASN1_TIME_print(bio, X509_get_notAfter(peer_cert));
  BIO_get_mem_ptr(bio, &mem);
  lua_pushstring(L, "valid_to");
  lua_pushlstring(L, mem->data, mem->length);
  lua_settable(L, -3);
  BIO_free(bio);

  if (X509_digest(peer_cert, EVP_sha1(), md, &md_size)) {
    const char hex[] = "0123456789ABCDEF";
    char fingerprint[EVP_MAX_MD_SIZE * 3];

    for (i=0; i<md_size; i++) {
      fingerprint[3*i] = hex[(md[i] & 0xf0) >> 4];
      fingerprint[(3*i)+1] = hex[(md[i] & 0x0f)];
      fingerprint[(3*i)+2] = ':';
    }

    if (md_size > 0) {
      fingerprint[(3*(md_size-1))+2] = '\0';
    }
    else {
      fingerprint[0] = '\0';
    }

    lua_pushstring(L, "fingerprint");
    lua_pushstring(L, fingerprint);
    lua_settable(L, -3);
  }


  eku = (STACK_OF(ASN1_OBJECT) *)X509_get_ext_d2i(peer_cert, NID_ext_key_usage,
                                                  NULL, NULL);
  if (eku != NULL) {
    char buf[256];
    lua_pushstring(L, "ext_key_usage");
    lua_newtable(L);
    for (i = 0; i < sk_ASN1_OBJECT_num(eku); i++) {
      memset(buf, 0, sizeof(buf));
      OBJ_obj2txt(buf, sizeof(buf) - 1, sk_ASN1_OBJECT_value(eku, i), 1);
      lua_pushnumber(L, i + 1);
      lua_pushstring(L, buf);
      lua_settable(L, -3);
    }
    sk_ASN1_OBJECT_pop_free(eku, ASN1_OBJECT_free);
    lua_settable(L, -3);
  }

  BIO_free(bio);
  X509_free(peer_cert);
  return 1;
}

static int
tls_conn_gc(lua_State *L) {
  return tls_conn_close(L);
}

static const luaL_reg tls_conn_lib[] = {
  {"encIn", tls_conn_enc_in},
  {"encOut", tls_conn_enc_out},
  {"encPending", tls_conn_enc_pending},
  {"getError", tls_conn_get_error},
  {"clearError", tls_conn_clear_error},
  {"clearOut", tls_conn_clear_out},
  {"clearIn", tls_conn_clear_in},
  {"clearPending", tls_conn_clear_pending},
  {"getPeerCertificate", tls_conn_get_peer_certificate},
  {"getCurrentCipher", tls_conn_get_current_cipher},
/*
  {"getSession", tls_conn_get_session},
  {"setSession", tls_conn_set_session},
*/
  {"isInitFinished", tls_conn_is_init_finished},
  {"shutdown", tls_conn_shutdown},
  {"start", tls_conn_start},
  {"verifyError", tls_conn_verify_error},
  {"close", tls_conn_close},
  {"__gc", tls_conn_gc},
  {NULL, NULL}
};

int
luvit__lua_tls_conn_init(lua_State *L)
{
  luaL_newmetatable(L, TLS_CONNECTION_HANDLE);
  lua_pushliteral(L, "__index");
  lua_pushvalue(L, -2);  /* push metatable */
  lua_rawset(L, -3);  /* metatable.__index = metatable */
  luaL_openlib(L, NULL, tls_conn_lib, 0);
  lua_pushvalue(L, -1);
  return 0;
}
