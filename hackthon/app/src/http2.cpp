#ifndef __http2_cpp__
#define __http2_cpp__

#include "http2.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

http2_session::http2_session(bool server_side, handler_t handler)
    : m_handler(std::move(handler)) {

  nghttp2_session_callbacks *cbs = nullptr;
  if (nghttp2_session_callbacks_new(&cbs) != 0)
    throw std::runtime_error("nghttp2_session_callbacks_new failed");

  nghttp2_session_callbacks_set_on_begin_headers_callback(cbs,
                                                          on_begin_headers);
  nghttp2_session_callbacks_set_on_header_callback(cbs, on_header);
  nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, on_frame_recv);
  nghttp2_session_callbacks_set_on_stream_close_callback(cbs, on_stream_close);
  nghttp2_session_callbacks_set_on_data_chunk_recv_callback(
      cbs, on_data_chunk_recv);

  int rc = server_side
               ? nghttp2_session_server_new(&m_session, cbs, this)
               : nghttp2_session_client_new(&m_session, cbs, this);
  nghttp2_session_callbacks_del(cbs);

  if (rc != 0)
    throw std::runtime_error(std::string("nghttp2_session_new: ") +
                             nghttp2_strerror(rc));

  // Both sides send their initial SETTINGS frame immediately.
  const nghttp2_settings_entry settings[] = {
      {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100},
  };
  rc = nghttp2_submit_settings(m_session, NGHTTP2_FLAG_NONE, settings, 1);
  if (rc != 0)
    throw std::runtime_error(std::string("nghttp2_submit_settings: ") +
                             nghttp2_strerror(rc));
}

http2_session::~http2_session() {
  if (m_session) {
    nghttp2_session_del(m_session);
    m_session = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Transport interface
// ---------------------------------------------------------------------------

ssize_t http2_session::recv(const uint8_t *data, size_t len) {
  return nghttp2_session_mem_recv(m_session, data, len);
}

std::string http2_session::drain_send_buf() {
  std::string out;
  const uint8_t *buf = nullptr;
  ssize_t n;
  while ((n = nghttp2_session_mem_send(m_session, &buf)) > 0)
    out.append(reinterpret_cast<const char *>(buf), static_cast<size_t>(n));
  return out;
}

bool http2_session::want_read() const {
  return nghttp2_session_want_read(m_session) != 0;
}

bool http2_session::want_write() const {
  return nghttp2_session_want_write(m_session) != 0;
}

// ---------------------------------------------------------------------------
// Submitting responses (server) and requests (client)
// ---------------------------------------------------------------------------

// make_nv for string-literal names (static duration — pointer is always valid).
nghttp2_nv http2_session::make_nv(const char *name, const std::string &value) {
  return {const_cast<uint8_t *>(reinterpret_cast<const uint8_t *>(name)),
          const_cast<uint8_t *>(
              reinterpret_cast<const uint8_t *>(value.data())),
          std::strlen(name), value.size(), NGHTTP2_NV_FLAG_NONE};
}

// make_nv for runtime string names (e.g. user-supplied header keys).
nghttp2_nv http2_session::make_nv(const std::string &name,
                                  const std::string &value) {
  return {const_cast<uint8_t *>(
              reinterpret_cast<const uint8_t *>(name.data())),
          const_cast<uint8_t *>(
              reinterpret_cast<const uint8_t *>(value.data())),
          name.size(), value.size(), NGHTTP2_NV_FLAG_NONE};
}

int http2_session::submit_response(
    int32_t stream_id, int status,
    const std::vector<std::pair<std::string, std::string>> &extra_headers,
    const std::string &body,
    bool with_trailers) {

  auto &ctx = m_streams[stream_id];
  ctx.resp_body = body;
  ctx.trailer_mode = with_trailers;

  const std::string status_str = std::to_string(status);
  std::vector<nghttp2_nv> nva;
  nva.push_back(make_nv(":status", status_str));
  for (const auto &[k, v] : extra_headers)
    nva.push_back(make_nv(k, v));

  // Use nullptr provider only when there is no body and no trailers follow
  // (nullptr causes nghttp2 to set END_STREAM on the HEADERS frame, which
  // would close the stream before we can send the trailing HEADERS).
  if (body.empty() && !with_trailers) {
    return nghttp2_submit_response(m_session, stream_id, nva.data(), nva.size(),
                                   nullptr);
  }

  // source.ptr points to the stream_ctx so the provider can also read
  // trailer_mode (changed from &ctx.resp_body in the original design).
  nghttp2_data_provider prd{};
  prd.source.ptr = &ctx;
  prd.read_callback = response_body_read;
  return nghttp2_submit_response(m_session, stream_id, nva.data(), nva.size(),
                                 &prd);
}

int http2_session::submit_trailer(
    int32_t stream_id,
    const std::vector<std::pair<std::string, std::string>> &trailers) {
  std::vector<nghttp2_nv> nva;
  for (const auto &[k, v] : trailers)
    nva.push_back(make_nv(k, v));
  return nghttp2_submit_trailer(m_session, stream_id, nva.data(), nva.size());
}

int32_t http2_session::submit_request(
    const std::string &method, const std::string &path,
    const std::string &authority, const std::string &scheme,
    const std::vector<std::pair<std::string, std::string>> &extra_headers,
    const std::string &body) {

  std::vector<nghttp2_nv> nva;
  nva.push_back(make_nv(":method", method));
  nva.push_back(make_nv(":path", path));
  nva.push_back(make_nv(":scheme", scheme));
  nva.push_back(make_nv(":authority", authority));
  for (const auto &[k, v] : extra_headers)
    nva.push_back(make_nv(k, v));

  if (body.empty()) {
    return nghttp2_submit_request(m_session, nullptr, nva.data(), nva.size(),
                                  nullptr, nullptr);
  }

  // Heap-allocate the body so it outlives this call; response_body_read
  // deletes it once fully consumed.
  auto *body_copy = new std::string(body);
  nghttp2_data_provider prd{};
  prd.source.ptr = body_copy;
  prd.read_callback = request_body_read;
  const int32_t sid = nghttp2_submit_request(m_session, nullptr, nva.data(),
                                             nva.size(), &prd, nullptr);
  if (sid < 0) {
    delete body_copy;
  }
  return sid;
}

// ---------------------------------------------------------------------------
// Static callbacks
// ---------------------------------------------------------------------------

int http2_session::on_begin_headers(nghttp2_session *,
                                    const nghttp2_frame *frame,
                                    void *user_data) {
  auto *self = static_cast<http2_session *>(user_data);
  if (frame->hd.type == NGHTTP2_HEADERS &&
      (frame->headers.cat == NGHTTP2_HCAT_REQUEST ||
       frame->headers.cat == NGHTTP2_HCAT_RESPONSE)) {
    self->m_streams.try_emplace(frame->hd.stream_id);
  }
  return 0;
}

int http2_session::on_header(nghttp2_session *, const nghttp2_frame *frame,
                             const uint8_t *name, size_t namelen,
                             const uint8_t *value, size_t valuelen,
                             uint8_t /* flags */, void *user_data) {
  auto *self = static_cast<http2_session *>(user_data);

  if (frame->hd.type != NGHTTP2_HEADERS)
    return 0;
  // Accept request, response, and trailing-HEADERS (NGHTTP2_HCAT_HEADERS)
  // categories — the last is used by gRPC to deliver grpc-status trailers.
  if (frame->headers.cat != NGHTTP2_HCAT_REQUEST &&
      frame->headers.cat != NGHTTP2_HCAT_RESPONSE &&
      frame->headers.cat != NGHTTP2_HCAT_HEADERS)
    return 0;

  auto it = self->m_streams.find(frame->hd.stream_id);
  if (it == self->m_streams.end())
    return 0;

  const std::string k(reinterpret_cast<const char *>(name), namelen);
  const std::string v(reinterpret_cast<const char *>(value), valuelen);
  auto &req = it->second.req;

  if (k == ":method")
    req.method = v;
  else if (k == ":path")
    req.path = v;
  else if (k == ":authority")
    req.authority = v;
  else if (k == ":scheme")
    req.scheme = v;
  else if (k == ":status")
    req.status = std::stoi(v);
  else
    req.headers[k] = v;

  return 0;
}

int http2_session::on_data_chunk_recv(nghttp2_session *, uint8_t /* flags */,
                                      int32_t stream_id, const uint8_t *data,
                                      size_t len, void *user_data) {
  auto *self = static_cast<http2_session *>(user_data);
  auto it = self->m_streams.find(stream_id);
  if (it != self->m_streams.end())
    it->second.req.body.append(reinterpret_cast<const char *>(data), len);
  return 0;
}

int http2_session::on_frame_recv(nghttp2_session *, const nghttp2_frame *frame,
                                 void *user_data) {
  auto *self = static_cast<http2_session *>(user_data);

  if (!(frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
    return 0;

  const auto type = frame->hd.type;
  if (type != NGHTTP2_HEADERS && type != NGHTTP2_DATA)
    return 0;

  auto it = self->m_streams.find(frame->hd.stream_id);
  if (it != self->m_streams.end() && self->m_handler)
    self->m_handler(frame->hd.stream_id, it->second.req);

  return 0;
}

int http2_session::on_stream_close(nghttp2_session *, int32_t stream_id,
                                   uint32_t /* error_code */,
                                   void *user_data) {
  auto *self = static_cast<http2_session *>(user_data);
  self->m_streams.erase(stream_id);
  return 0;
}

// ---------------------------------------------------------------------------
// Data providers
// ---------------------------------------------------------------------------

ssize_t http2_session::response_body_read(nghttp2_session *, int32_t,
                                          uint8_t *buf, size_t length,
                                          uint32_t *data_flags,
                                          nghttp2_data_source *source,
                                          void *) {
  auto *ctx = static_cast<stream_ctx *>(source->ptr);
  auto &body = ctx->resp_body;
  const size_t n = std::min(length, body.size());
  std::memcpy(buf, body.data(), n);
  body.erase(0, n);
  if (body.empty()) {
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    // When trailer_mode is set, tell nghttp2 NOT to add END_STREAM on the
    // DATA frame — we will close the stream via submit_trailer() instead.
    if (ctx->trailer_mode)
      *data_flags |= NGHTTP2_DATA_FLAG_NO_END_STREAM;
  }
  return static_cast<ssize_t>(n);
}

ssize_t http2_session::request_body_read(nghttp2_session *, int32_t,
                                         uint8_t *buf, size_t length,
                                         uint32_t *data_flags,
                                         nghttp2_data_source *source, void *) {
  auto *body = static_cast<std::string *>(source->ptr);
  const size_t n = std::min(length, body->size());
  std::memcpy(buf, body->data(), n);
  body->erase(0, n);
  if (body->empty()) {
    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    delete body; // heap-allocated in submit_request
  }
  return static_cast<ssize_t>(n);
}

#endif // __http2_cpp__
