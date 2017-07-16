/*
 * ngtcp2
 *
 * Copyright (c) 2017 ngtcp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <cstdlib>
#include <cassert>
#include <iostream>
#include <algorithm>
#include <memory>

#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include <openssl/bio.h>
#include <openssl/err.h>

#include "server.h"
#include "template.h"
#include "network.h"
#include "debug.h"
#include "util.h"
#include "crypto.h"

using namespace ngtcp2;

namespace {
auto randgen = util::make_mt19937();
} // namespace

namespace {
int bio_write(BIO *b, const char *buf, int len) {
  BIO_clear_retry_flags(b);

  auto h = static_cast<Handler *>(BIO_get_data(b));

  h->write_server_handshake(reinterpret_cast<const uint8_t *>(buf), len);

  return len;
}
} // namespace

namespace {
int bio_read(BIO *b, char *buf, int len) {
  BIO_clear_retry_flags(b);

  auto h = static_cast<Handler *>(BIO_get_data(b));

  len = h->read_client_handshake(reinterpret_cast<uint8_t *>(buf), len);
  if (len == 0) {
    BIO_set_retry_read(b);
    return -1;
  }

  return len;
}
} // namespace

namespace {
int bio_puts(BIO *b, const char *str) { return bio_write(b, str, strlen(str)); }
} // namespace

namespace {
int bio_gets(BIO *b, char *buf, int len) { return -1; }
} // namespace

namespace {
long bio_ctrl(BIO *b, int cmd, long num, void *ptr) {
  switch (cmd) {
  case BIO_CTRL_FLUSH:
    return 1;
  }

  return 0;
}
} // namespace

namespace {
int bio_create(BIO *b) {
  BIO_set_init(b, 1);
  return 1;
}
} // namespace

namespace {
int bio_destroy(BIO *b) {
  if (b == nullptr) {
    return 0;
  }

  return 1;
}
} // namespace

namespace {
BIO_METHOD *create_bio_method() {
  static auto meth = BIO_meth_new(BIO_TYPE_FD, "bio");
  BIO_meth_set_write(meth, bio_write);
  BIO_meth_set_read(meth, bio_read);
  BIO_meth_set_puts(meth, bio_puts);
  BIO_meth_set_gets(meth, bio_gets);
  BIO_meth_set_ctrl(meth, bio_ctrl);
  BIO_meth_set_create(meth, bio_create);
  BIO_meth_set_destroy(meth, bio_destroy);
  return meth;
}
} // namespace

namespace {
void timeoutcb(struct ev_loop *loop, ev_timer *w, int revents) {
  auto h = static_cast<Handler *>(w->data);

  debug::print_timestamp();
  std::cerr << "Timeout" << std::endl;

  delete h;
}
} // namespace

Handler::Handler(struct ev_loop *loop, SSL_CTX *ssl_ctx)
    : remote_addr_{},
      max_pktlen_(0),
      loop_(loop),
      ssl_ctx_(ssl_ctx),
      ssl_(nullptr),
      fd_(-1),
      ncread_(0),
      nsread_(0),
      conn_(nullptr),
      crypto_ctx_{} {
  ev_timer_init(&timer_, timeoutcb, 5., 0.);
  timer_.data = this;
}

Handler::~Handler() {
  debug::print_timestamp();
  std::cerr << "Closing QUIC connection" << std::endl;

  ev_timer_stop(loop_, &timer_);

  if (conn_) {
    ngtcp2_conn_del(conn_);
  }

  if (ssl_) {
    SSL_free(ssl_);
  }
}

namespace {
ssize_t send_server_cleartext(ngtcp2_conn *conn, uint32_t flags,
                              uint64_t *ppkt_num, const uint8_t **pdest,
                              void *user_data) {
  auto h = static_cast<Handler *>(user_data);

  if (h->tls_handshake() != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  if (ppkt_num) {
    *ppkt_num = std::uniform_int_distribution<uint64_t>(
        0, std::numeric_limits<int32_t>::max())(randgen);
  }

  auto len = h->read_server_handshake(pdest);

  // If Client Initial does not have complete ClientHello, then drop
  // connection.
  if (ppkt_num && len == 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return len;
}
} // namespace

namespace {
int handshake_completed(ngtcp2_conn *conn, void *user_data) {
  auto h = static_cast<Handler *>(user_data);

  debug::handshake_completed(conn, user_data);

  if (h->setup_crypto_context() != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

namespace {
ssize_t do_encrypt(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                   const uint8_t *plaintext, size_t plaintextlen,
                   const uint8_t *key, size_t keylen, const uint8_t *nonce,
                   size_t noncelen, const uint8_t *ad, size_t adlen,
                   void *user_data) {
  auto h = static_cast<Handler *>(user_data);

  auto nwrite = h->encrypt_data(dest, destlen, plaintext, plaintextlen, key,
                                keylen, nonce, noncelen, ad, adlen);
  if (nwrite < 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return nwrite;
}
} // namespace

namespace {
ssize_t do_decrypt(ngtcp2_conn *conn, uint8_t *dest, size_t destlen,
                   const uint8_t *ciphertext, size_t ciphertextlen,
                   const uint8_t *key, size_t keylen, const uint8_t *nonce,
                   size_t noncelen, const uint8_t *ad, size_t adlen,
                   void *user_data) {
  auto h = static_cast<Handler *>(user_data);

  auto nwrite = h->decrypt_data(dest, destlen, ciphertext, ciphertextlen, key,
                                keylen, nonce, noncelen, ad, adlen);
  if (nwrite < 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return nwrite;
}
} // namespace

namespace {
int recv_handshake_data(ngtcp2_conn *conn, const uint8_t *data, size_t datalen,
                        void *user_data) {
  auto h = static_cast<Handler *>(user_data);

  h->write_client_handshake(data, datalen);

  if (h->tls_handshake() != 0) {
    return NGTCP2_ERR_CALLBACK_FAILURE;
  }

  return 0;
}
} // namespace

int Handler::init(int fd, const sockaddr *sa, socklen_t salen) {
  int rv;

  remote_addr_.len = salen;
  memcpy(&remote_addr_.su.sa, sa, salen);

  switch (remote_addr_.su.storage.ss_family) {
  case AF_INET:
    max_pktlen_ = NGTCP2_MAX_PKTLEN_IPV4;
    break;
  case AF_INET6:
    max_pktlen_ = NGTCP2_MAX_PKTLEN_IPV6;
    break;
  default:
    return -1;
  }

  fd_ = fd;
  ssl_ = SSL_new(ssl_ctx_);
  auto bio = BIO_new(create_bio_method());
  BIO_set_data(bio, this);
  SSL_set_bio(ssl_, bio, bio);
  SSL_set_app_data(ssl_, this);
  SSL_set_accept_state(ssl_);

  auto callbacks = ngtcp2_conn_callbacks{
      nullptr,
      nullptr,
      send_server_cleartext,
      recv_handshake_data,
      debug::send_pkt,
      debug::send_frame,
      debug::recv_pkt,
      debug::recv_frame,
      handshake_completed,
      nullptr,
      do_encrypt,
      do_decrypt,
  };

  auto conn_id = std::uniform_int_distribution<uint64_t>(
      0, std::numeric_limits<uint64_t>::max())(randgen);

  rv = ngtcp2_conn_server_new(&conn_, conn_id, NGTCP2_PROTO_VERSION, &callbacks,
                              this);
  if (rv != 0) {
    std::cerr << "ngtcp2_conn_server_new: " << ngtcp2_strerror(rv) << std::endl;
    return -1;
  }

  ev_timer_start(loop_, &timer_);

  return 0;
}

int Handler::tls_handshake() {
  ERR_clear_error();

  auto rv = SSL_do_handshake(ssl_);
  if (rv <= 0) {
    auto err = SSL_get_error(ssl_, rv);
    switch (err) {
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_WANT_WRITE:
      return 0;
    case SSL_ERROR_SSL:
      std::cerr << "TLS handshake error: "
                << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
      return -1;
    default:
      std::cerr << "TLS handshake error: " << err << std::endl;
      return -1;
    }
  }

  // SSL_do_handshake returns 1 if TLS handshake has completed.  With
  // boringSSL, it may return 1 if we have 0-RTT early data.  This is
  // a problem, but for First Implementation draft, 0-RTT early data
  // is out of interest.
  ngtcp2_conn_handshake_completed(conn_);

  return 0;
}

void Handler::write_server_handshake(const uint8_t *data, size_t datalen) {
  std::copy_n(data, datalen, std::back_inserter(chandshake_));
}

size_t Handler::read_server_handshake(const uint8_t **pdest) {
  auto n = chandshake_.size() - ncread_;
  *pdest = chandshake_.data() + ncread_;
  ncread_ = chandshake_.size();
  return n;
}

size_t Handler::read_client_handshake(uint8_t *buf, size_t buflen) {
  auto n = std::min(buflen, shandshake_.size() - nsread_);
  std::copy_n(std::begin(shandshake_) + nsread_, n, buf);
  nsread_ += n;
  return n;
}

void Handler::write_client_handshake(const uint8_t *data, size_t datalen) {
  std::copy_n(data, datalen, std::back_inserter(shandshake_));
}

int Handler::setup_crypto_context() {
  int rv;

  rv = crypto::negotiated_prf(crypto_ctx_, ssl_);
  if (rv != 0) {
    return -1;
  }
  rv = crypto::negotiated_aead(crypto_ctx_, ssl_);
  if (rv != 0) {
    return -1;
  }

  auto length = EVP_MD_size(crypto_ctx_.prf);

  crypto_ctx_.secretlen = length;

  rv = crypto::export_server_secret(crypto_ctx_.tx_secret.data(),
                                    crypto_ctx_.secretlen, ssl_);
  if (rv != 0) {
    return -1;
  }

  std::array<uint8_t, 64> key{}, iv{};

  auto keylen = crypto::derive_packet_protection_key(
      key.data(), key.size(), crypto_ctx_.tx_secret.data(),
      crypto_ctx_.secretlen, crypto_ctx_);
  if (rv != 0) {
    return -1;
  }

  auto ivlen = crypto::derive_packet_protection_iv(
      iv.data(), iv.size(), crypto_ctx_.tx_secret.data(), crypto_ctx_.secretlen,
      crypto_ctx_);
  if (rv != 0) {
    return -1;
  }

  ngtcp2_conn_update_tx_keys(conn_, key.data(), keylen, iv.data(), ivlen);

  rv = crypto::export_client_secret(crypto_ctx_.rx_secret.data(),
                                    crypto_ctx_.secretlen, ssl_);
  if (rv != 0) {
    return -1;
  }

  keylen = crypto::derive_packet_protection_key(
      key.data(), key.size(), crypto_ctx_.rx_secret.data(),
      crypto_ctx_.secretlen, crypto_ctx_);
  if (rv != 0) {
    return -1;
  }

  ivlen = crypto::derive_packet_protection_iv(
      iv.data(), iv.size(), crypto_ctx_.rx_secret.data(), crypto_ctx_.secretlen,
      crypto_ctx_);
  if (rv != 0) {
    return -1;
  }

  ngtcp2_conn_update_rx_keys(conn_, key.data(), keylen, iv.data(), ivlen);

  ngtcp2_conn_set_aead_overhead(conn_, crypto::aead_max_overhead(crypto_ctx_));

  return 0;
}

ssize_t Handler::encrypt_data(uint8_t *dest, size_t destlen,
                              const uint8_t *plaintext, size_t plaintextlen,
                              const uint8_t *key, size_t keylen,
                              const uint8_t *nonce, size_t noncelen,
                              const uint8_t *ad, size_t adlen) {
  return crypto::encrypt(dest, destlen, plaintext, plaintextlen, crypto_ctx_,
                         key, keylen, nonce, noncelen, ad, adlen);
}

ssize_t Handler::decrypt_data(uint8_t *dest, size_t destlen,
                              const uint8_t *ciphertext, size_t ciphertextlen,
                              const uint8_t *key, size_t keylen,
                              const uint8_t *nonce, size_t noncelen,
                              const uint8_t *ad, size_t adlen) {
  return crypto::decrypt(dest, destlen, ciphertext, ciphertextlen, crypto_ctx_,
                         key, keylen, nonce, noncelen, ad, adlen);
}

int Handler::feed_data(uint8_t *data, size_t datalen) {
  int rv;

  rv = ngtcp2_conn_recv(conn_, data, datalen, util::timestamp());
  if (rv != 0) {
    std::cerr << "ngtcp2_conn_recv: " << ngtcp2_strerror(rv) << std::endl;
    return -1;
  }

  return 0;
}

int Handler::on_read(uint8_t *data, size_t datalen) {
  if (feed_data(data, datalen) != 0) {
    return -1;
  }

  return on_write();
}

int Handler::on_write() {
  std::array<uint8_t, NGTCP2_MAX_PKTLEN_IPV4> buf;

  assert(buf.size() >= max_pktlen_);

  for (;;) {
    auto n = ngtcp2_conn_send(conn_, buf.data(), buf.size(), util::timestamp());
    if (n < 0) {
      std::cerr << "ngtcp2_conn_send: " << ngtcp2_strerror(n) << std::endl;
      return -1;
    }
    if (n == 0) {
      return 0;
    }

    auto nwrite =
        sendto(fd_, buf.data(), n, 0, &remote_addr_.su.sa, remote_addr_.len);
    if (nwrite == -1) {
      std::cerr << "sendto: " << strerror(errno) << std::endl;
      return -1;
    }
  }
}

namespace {
void swritecb(struct ev_loop *loop, ev_io *w, int revents) {}
} // namespace

namespace {
void sreadcb(struct ev_loop *loop, ev_io *w, int revents) {
  auto s = static_cast<Server *>(w->data);

  s->on_read();
}
} // namespace

Server::Server(struct ev_loop *loop, SSL_CTX *ssl_ctx)
    : loop_(loop), ssl_ctx_(ssl_ctx), fd_(-1) {
  ev_io_init(&wev_, swritecb, 0, EV_WRITE);
  ev_io_init(&rev_, sreadcb, 0, EV_READ);
  wev_.data = this;
  rev_.data = this;
}

Server::~Server() {
  ev_io_stop(loop_, &rev_);
  ev_io_stop(loop_, &wev_);

  if (fd_ != -1) {
    close(fd_);
  }
}

int Server::init(int fd) {
  fd_ = fd;

  ev_io_set(&wev_, fd_, EV_WRITE);
  ev_io_set(&rev_, fd_, EV_READ);

  ev_io_start(loop_, &rev_);

  return 0;
}

std::string create_conn_key(const sockaddr *sa, socklen_t salen) {
  int rv;
  std::array<char, NI_MAXHOST> host;
  std::array<char, NI_MAXSERV> serv;

  rv = getnameinfo(sa, salen, host.data(), host.size(), serv.data(),
                   serv.size(), NI_NUMERICHOST | NI_NUMERICSERV);
  if (rv != 0) {
    std::cerr << "getnameinfo: " << gai_strerror(rv) << std::endl;
    return "";
  }

  return "[" + std::string{host.data()} + "]:" + serv.data();
}

int Server::on_read() {
  sockaddr_union su;
  socklen_t addrlen = sizeof(su);
  std::array<uint8_t, 64_k> buf;
  int rv;
  ngtcp2_pkt_hd hd;

  auto nread =
      recvfrom(fd_, buf.data(), buf.size(), MSG_DONTWAIT, &su.sa, &addrlen);
  if (nread == -1) {
    std::cerr << "recvfrom: " << strerror(errno) << std::endl;
    // TODO Handle running out of fd
    return 0;
  }

  auto conn_key = create_conn_key(&su.sa, addrlen);
  if (conn_key.empty()) {
    return 0;
  }

  auto handler_it = handlers_.find(conn_key);
  if (handler_it == std::end(handlers_)) {
    switch (su.storage.ss_family) {
    case AF_INET:
      if (nread < NGTCP2_MAX_PKTLEN_IPV4) {
        std::cerr << "IPv4 packet is too short: " << nread << " < "
                  << NGTCP2_MAX_PKTLEN_IPV4 << std::endl;
        return 0;
      }
      break;
    case AF_INET6:
      if (nread < NGTCP2_MAX_PKTLEN_IPV6) {
        std::cerr << "IPv6 packet is too short: " << nread << " < "
                  << NGTCP2_MAX_PKTLEN_IPV6 << std::endl;
        return 0;
      }
      break;
    }

    rv = ngtcp2_accept(&hd, buf.data(), nread);
    if (rv == -1) {
      std::cerr << "Unexpected packet received" << std::endl;
      return 0;
    }
    if (rv == 1) {
      std::cerr << "Unsupported version: Send Version Negotiation" << std::endl;
      send_version_negotiation(&hd, &su.sa, addrlen);
      return 0;
    }

    if ((buf[0] & 0x7f) != NGTCP2_PKT_CLIENT_INITIAL) {
      return 0;
    }

    auto h = std::make_unique<Handler>(loop_, ssl_ctx_);
    h->init(fd_, &su.sa, addrlen);

    if (h->on_read(buf.data(), nread) != 0) {
      return 0;
    }

    handlers_.insert(std::make_pair(conn_key, std::move(h)));
    return 0;
  }

  auto h = (*handler_it).second.get();
  if (h->on_read(buf.data(), nread) != 0) {
    handlers_.erase(conn_key);
  }

  return 0;
}

namespace {
uint32_t generate_reserved_vesrion(const sockaddr *sa, socklen_t salen,
                                   uint32_t version) {
  uint32_t h = 0x811C9DC5u;
  const uint8_t *p = (const uint8_t *)sa;
  const uint8_t *ep = p + salen;
  for (; p != ep; ++p) {
    h ^= *p;
    h *= 0x01000193u;
  }
  version = htonl(version);
  p = (const uint8_t *)&version;
  ep = p + sizeof(version);
  for (; p != ep; ++p) {
    h ^= *p;
    h *= 0x01000193u;
  }
  h &= 0xf0f0f0f0u;
  h |= 0x0a0a0a0au;
  return h;
}
} // namespace

int Server::send_version_negotiation(const ngtcp2_pkt_hd *chd,
                                     const sockaddr *sa, socklen_t salen) {
  std::array<uint8_t, 256> buf;
  ngtcp2_upe *upe;
  ngtcp2_pkt_hd hd;
  uint32_t reserved_ver;
  uint32_t sv[2];
  size_t pktlen;
  ssize_t nwrite;
  int rv;

  hd.type = NGTCP2_PKT_VERSION_NEGOTIATION;
  hd.flags = NGTCP2_PKT_FLAG_LONG_FORM;
  hd.conn_id = chd->conn_id;
  hd.pkt_num = chd->pkt_num;
  hd.version = chd->version;

  reserved_ver = generate_reserved_vesrion(sa, salen, hd.version);

  sv[0] = reserved_ver;
  sv[1] = NGTCP2_PROTO_VERSION;

  rv = ngtcp2_upe_new(&upe, buf.data(), buf.size());
  if (rv != 0) {
    std::cerr << "ngtcp2_upe_new: " << ngtcp2_strerror(rv) << std::endl;
    return -1;
  }

  auto upe_d = defer(ngtcp2_upe_del, upe);

  rv = ngtcp2_upe_encode_hd(upe, &hd);
  if (rv != 0) {
    return -1;
  }

  rv = ngtcp2_upe_encode_version_negotiation(upe, sv, array_size(sv));
  if (rv != 0) {
    std::cerr << "ngtcp2_upe_encode_version_negotiation: "
              << ngtcp2_strerror(rv) << std::endl;
    return -1;
  }

  pktlen = ngtcp2_upe_final(upe, NULL);

  nwrite = sendto(fd_, buf.data(), pktlen, 0, sa, salen);
  if (nwrite == -1) {
    std::cerr << "sendto: " << strerror(errno) << std::endl;
    return -1;
  }

  return 0;
}

namespace {
SSL_CTX *create_ssl_ctx(const char *private_key_file, const char *cert_file) {
  auto ssl_ctx = SSL_CTX_new(TLS_method());

  constexpr auto ssl_opts = (SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS) |
                            SSL_OP_SINGLE_ECDH_USE |
                            SSL_OP_CIPHER_SERVER_PREFERENCE;

  SSL_CTX_set_options(ssl_ctx, ssl_opts);
  SSL_CTX_set1_curves_list(ssl_ctx, "p-256");
  SSL_CTX_set_mode(ssl_ctx, SSL_MODE_RELEASE_BUFFERS);

  SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_3_VERSION);
  SSL_CTX_set_max_proto_version(ssl_ctx, TLS1_3_VERSION);

  SSL_CTX_set_default_verify_paths(ssl_ctx);

  if (SSL_CTX_use_PrivateKey_file(ssl_ctx, private_key_file,
                                  SSL_FILETYPE_PEM) != 1) {
    std::cerr << "SSL_CTX_use_PrivateKey_file: "
              << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
    goto fail;
  }

  if (SSL_CTX_use_certificate_chain_file(ssl_ctx, cert_file) != 1) {
    std::cerr << "SSL_CTX_use_certificate_file: "
              << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
    goto fail;
  }

  if (SSL_CTX_check_private_key(ssl_ctx) != 1) {
    std::cerr << "SSL_CTX_check_private_key: "
              << ERR_error_string(ERR_get_error(), nullptr) << std::endl;
    goto fail;
  }

  return ssl_ctx;

fail:
  SSL_CTX_free(ssl_ctx);
  return nullptr;
}
} // namespace

namespace {
int create_sock(const char *addr, const char *port) {
  addrinfo hints{};
  addrinfo *res, *rp;
  int rv;

  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE;

  rv = getaddrinfo(addr, port, &hints, &res);
  if (rv != 0) {
    std::cerr << "getaddrinfo: " << gai_strerror(rv) << std::endl;
    return -1;
  }

  auto res_d = defer(freeaddrinfo, res);

  int fd = -1;

  for (rp = res; rp; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd == -1) {
      continue;
    }

    if (bind(fd, rp->ai_addr, rp->ai_addrlen) != -1) {
      break;
    }

    close(fd);
  }

  if (!rp) {
    std::cerr << "Could not bind" << std::endl;
    return -1;
  }

  auto val = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val,
                 static_cast<socklen_t>(sizeof(val))) == -1) {
    return -1;
  }

  return fd;
}

} // namespace

namespace {
int serve(Server &s, const char *addr, const char *port) {
  int rv;

  auto fd = create_sock(addr, port);
  if (fd == -1) {
    return -1;
  }

  if (s.init(fd) != 0) {
    return -1;
  }

  ev_run(EV_DEFAULT, 0);

  return 0;
}
} // namespace

namespace {
void print_usage() {
  std::cerr << "Usage: server ADDR PORT PRIVATE_KEY_FILE CERTIFICATE_FILE"
            << std::endl;
}
} // namespace

int main(int argc, char **argv) {
  for (;;) {
    static int flag = 0;
    constexpr static option long_opts[] = {{nullptr, 0, nullptr, 0}};

    auto optidx = 0;
    auto c = getopt_long(argc, argv, "", long_opts, &optidx);
    if (c == -1) {
      break;
    }
    switch (c) {
    case '?':
      print_usage();
      exit(EXIT_FAILURE);
    default:
      break;
    };
  }

  if (argc - optind < 4) {
    std::cerr << "Too few arguments" << std::endl;
    print_usage();
    exit(EXIT_FAILURE);
  }

  auto addr = argv[optind++];
  auto port = argv[optind++];
  auto private_key_file = argv[optind++];
  auto cert_file = argv[optind++];

  auto ssl_ctx = create_ssl_ctx(private_key_file, cert_file);
  if (ssl_ctx == nullptr) {
    exit(EXIT_FAILURE);
  }

  auto ssl_ctx_d = defer(SSL_CTX_free, ssl_ctx);

  debug::reset_timestamp();

  if (isatty(STDOUT_FILENO)) {
    debug::set_color_output(true);
  }

  Server s(EV_DEFAULT, ssl_ctx);

  if (serve(s, addr, port) != 0) {
    exit(EXIT_FAILURE);
  }
}
