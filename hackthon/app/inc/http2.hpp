#ifndef __http2_hpp__
#define __http2_hpp__

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <nghttp2/nghttp2.h>
}

// http2_session wraps an nghttp2 server or client session.
//
// Transport is decoupled from protocol: callers feed raw bytes in via recv()
// and drain bytes to send via drain_send_buf(). This makes the class easy
// to unit-test with an in-memory loopback.
class http2_session {
public:
  struct request {
    std::string method;
    std::string path;
    std::string authority;
    std::string scheme;
    int status{0}; // non-zero for responses (:status pseudo-header)
    std::map<std::string, std::string> headers;
    std::string body;
  };

  // Fired when a complete request (server-side) or response (client-side)
  // has been received. The int32_t is the stream id.
  using handler_t = std::function<void(int32_t, const request &)>;

  explicit http2_session(bool server_side, handler_t handler = {});
  ~http2_session();

  http2_session(const http2_session &) = delete;
  http2_session &operator=(const http2_session &) = delete;

  // Feed bytes received from the network into nghttp2. Returns bytes consumed
  // or a negative nghttp2 error code.
  ssize_t recv(const uint8_t *data, size_t len);

  // Drain all bytes nghttp2 wants to send. Call after recv() or after
  // submitting a request/response.
  std::string drain_send_buf();

  // Server-side: submit an HTTP/2 response for stream_id.
  // Returns 0 on success or a negative nghttp2 error code.
  int submit_response(
      int32_t stream_id, int status,
      const std::vector<std::pair<std::string, std::string>> &extra_headers,
      const std::string &body);

  // Client-side: submit an HTTP/2 request.
  // Returns the new stream id (> 0) or a negative nghttp2 error code.
  int32_t submit_request(
      const std::string &method, const std::string &path,
      const std::string &authority, const std::string &scheme = "https",
      const std::vector<std::pair<std::string, std::string>> &extra_headers =
          {},
      const std::string &body = "");

  bool want_read() const;
  bool want_write() const;

private:
  struct stream_ctx {
    request req;
    std::string resp_body; // staging buffer for submit_response data provider
  };

  // Overload for string-literal names (static duration — no temporary created).
  static nghttp2_nv make_nv(const char *name, const std::string &value);
  // Overload for runtime string names (e.g. user-supplied header names).
  static nghttp2_nv make_nv(const std::string &name, const std::string &value);

  // nghttp2 session callbacks
  static int on_begin_headers(nghttp2_session *session,
                              const nghttp2_frame *frame, void *user_data);
  static int on_header(nghttp2_session *session, const nghttp2_frame *frame,
                       const uint8_t *name, size_t namelen,
                       const uint8_t *value, size_t valuelen, uint8_t flags,
                       void *user_data);
  static int on_frame_recv(nghttp2_session *session, const nghttp2_frame *frame,
                           void *user_data);
  static int on_stream_close(nghttp2_session *session, int32_t stream_id,
                             uint32_t error_code, void *user_data);
  static int on_data_chunk_recv(nghttp2_session *session, uint8_t flags,
                                int32_t stream_id, const uint8_t *data,
                                size_t len, void *user_data);

  // Data provider for response bodies (stream_ctx::resp_body)
  static ssize_t response_body_read(nghttp2_session *session, int32_t stream_id,
                                    uint8_t *buf, size_t length,
                                    uint32_t *data_flags,
                                    nghttp2_data_source *source,
                                    void *user_data);

  // Data provider for request bodies (heap-allocated std::string, self-deleting)
  static ssize_t request_body_read(nghttp2_session *session, int32_t stream_id,
                                   uint8_t *buf, size_t length,
                                   uint32_t *data_flags,
                                   nghttp2_data_source *source, void *user_data);

  nghttp2_session *m_session{nullptr};
  handler_t m_handler;
  std::unordered_map<int32_t, stream_ctx> m_streams;
};

#endif // __http2_hpp__
