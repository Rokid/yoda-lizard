#include <errno.h>
#include <string.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include "ssl-node.h"

namespace rokid {
namespace lizard {

const char* SSLNode::error_messages[] = {
  "ssl initialize failed",
  "ssl handshake failed",
  "ssl write failed",
  "ssl read failed",
  "socket not initialized",
  "read buffer size insufficient",
  "remote socket closed",
  "ssl read timeout",
};

static void set_node_error_by_errno(NodeError* err) {
  if (err) {
    err->code = errno;
    err->descript = strerror(errno);
  }
}

SSLNode::~SSLNode() {
  on_close();
}

void SSLNode::set_read_timeout(uint32_t tm) {
  read_timeout = tm;
  if (socket >= 0) {
    struct timeval tv;
    tv.tv_sec = tm / 1000;
    tv.tv_usec = (tm % 1000) * 1000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
}

static int my_net_recv(void* ctx, unsigned char* buf, size_t len) {
  int fd = *(int*)ctx;
  ssize_t ret = ::read(fd, buf, len);
  if (ret < 0) {
    // if socket read timeout, errno will be EAGAIN
    if (errno == EAGAIN)
      return POLARSSL_ERR_NET_WANT_READ;
    return POLARSSL_ERR_NET_RECV_FAILED;
  }
  return ret;
}

bool SSLNode::on_init(rokid::Uri& uri, NodeError* err) {
  static const char* pers = "lizard_ssl_node";

  memset(&ssl, 0, sizeof(ssl_context));
  entropy_init(&entropy);
  if(ctr_drbg_init(&ctr_drbg, entropy_func, &entropy,
        (const unsigned char *) pers, strlen(pers)) != 0) {
    set_node_error(err, SSL_INIT_FAILED);
    entropy_free(&entropy);
    return false;
  }
  if(ssl_init(&ssl) != 0) {
    set_node_error(err, SSL_INIT_FAILED);
    ctr_drbg_free(&ctr_drbg);
    entropy_free(&entropy);
    return false;
  }
  ssl_set_endpoint(&ssl, SSL_IS_CLIENT);
  ssl_set_rng(&ssl, ctr_drbg_random, &ctr_drbg);
  if (net_connect(&socket, uri.host.c_str(), uri.port)) {
    ssl_free(&ssl);
    ctr_drbg_free(&ctr_drbg);
    entropy_free(&entropy);
    set_node_error(err, SSL_INIT_FAILED);
    return false;
  }

  int r;
  ssl_set_bio(&ssl, my_net_recv, &socket, net_send, &socket);
  do {
    r = ssl_handshake(&ssl);
    // if (r == POLARSSL_ERR_NET_WANT_WRITE || r == POLARSSL_ERR_NET_WANT_READ)
    //   continue;
    if (r < 0) {
      set_node_error(err, SSL_HANDSHAKE_FAILED);
      net_close(socket);
      socket = -1;
      ssl_free(&ssl);
      ctr_drbg_free(&ctr_drbg);
      entropy_free(&entropy);
      return false;
    }
    break;
  } while (true);
  set_read_timeout(read_timeout);
  return true;
}

void SSLNode::set_node_error(NodeError* err, int32_t code) {
  if (err) {
    err->code = code;
    err->descript = error_messages[ERROR_CODE_BEGIN - code];
  }
}

int32_t SSLNode::on_write(Buffer& in, Buffer& out, NodeError* err) {
  int r;
  while (true) {
    r = ssl_write(&ssl, (unsigned char*)in.data + in.begin, in.size());
    if (r >= 0) {
      in.consume(r);
      if (in.empty())
        break;
    } else if (r != POLARSSL_ERR_NET_WANT_READ && r != POLARSSL_ERR_NET_WANT_WRITE) {
      set_node_error(err, SSL_WRITE_FAILED);
      return -1;
    }
  }
  return 0;
}

int32_t SSLNode::on_read(Buffer& out, NodeError* err,
    void* super_extra, void** extra) {
  if (socket < 0) {
    set_node_error(err, NOT_READY);
    return -1;
  }
  if (out.capacity == out.end) {
    set_node_error(err, INSUFF_READ_BUFFER);
    return -1;
  }

  int ret;
  do {
    ret = ssl_read(&ssl, (unsigned char*)out.data + out.begin, out.capacity);

    if (ret == POLARSSL_ERR_NET_WANT_READ) {
      set_node_error(err, SSL_READ_TIMEOUT);
      return -1;
    }

    if(ret < 0) {
      set_node_error(err, SSL_READ_FAILED);
      return -1;
    }

    if(ret == 0) {
      set_node_error(err, REMOTE_CLOSED);
      return -1;
    }

    out.obtain(ret);
    break;
  } while(true);

  return 0;
}

void SSLNode::on_close() {
  if (socket >= 0) {
    net_close(socket);
    ssl_free(&ssl);
    ctr_drbg_free(&ctr_drbg);
    entropy_free(&entropy);
    socket = -1;
  }
}

} // namespace lizard
} // namespace rokid