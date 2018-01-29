// Copyright 2018 The Chromium Authors. All rights reserved.
// Copyright 2018 klzgrad <kizdiv@gmail.com>. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/tools/naive/naive_client_connection.h"

#include <utility>

#include "base/bind.h"
#include "base/callback_helpers.h"
#include "base/logging.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/base/privacy_mode.h"
#include "net/http/http_network_session.h"
#include "net/proxy/proxy_config.h"
#include "net/proxy/proxy_info.h"
#include "net/proxy/proxy_list.h"
#include "net/proxy/proxy_service.h"
#include "net/socket/client_socket_handle.h"
#include "net/socket/client_socket_pool_manager.h"
#include "net/socket/stream_socket.h"
#include "net/spdy/chromium/spdy_session.h"
#include "net/tools/naive/socks5_server_socket.h"

namespace net {

namespace {
static const int kBufferSize = 64 * 1024;
}

NaiveClientConnection::NaiveClientConnection(
    int id,
    std::unique_ptr<StreamSocket> accepted_socket,
    HttpNetworkSession* session)
    : id_(id),
      next_state_(STATE_NONE),
      session_(session),
      net_log_(
          NetLogWithSource::Make(session->net_log(), NetLogSourceType::NONE)),
      client_socket_(
          std::make_unique<Socks5ServerSocket>(std::move(accepted_socket))),
      server_socket_handle_(std::make_unique<ClientSocketHandle>()),
      sockets_{client_socket_.get(), nullptr},
      errors_{OK, OK},
      write_pending_{false, false},
      early_pull_pending_(false),
      can_push_to_server_(false),
      early_pull_result_(ERR_IO_PENDING),
      full_duplex_(false),
      time_func_(&base::TimeTicks::Now),
      weak_ptr_factory_(this) {
  io_callback_ = base::Bind(&NaiveClientConnection::OnIOComplete,
                            weak_ptr_factory_.GetWeakPtr());
}

NaiveClientConnection::~NaiveClientConnection() {
  Disconnect();
}

int NaiveClientConnection::Connect(const CompletionCallback& callback) {
  DCHECK(client_socket_);
  DCHECK_EQ(next_state_, STATE_NONE);
  DCHECK(!connect_callback_);

  if (full_duplex_)
    return OK;

  next_state_ = STATE_CONNECT_CLIENT;

  int rv = DoLoop(OK);
  if (rv == ERR_IO_PENDING) {
    connect_callback_ = callback;
  }
  return rv;
}

void NaiveClientConnection::Disconnect() {
  full_duplex_ = false;
  // Closes server side first because latency is higher.
  if (server_socket_handle_->socket())
    server_socket_handle_->socket()->Disconnect();
  client_socket_->Disconnect();

  next_state_ = STATE_NONE;
  connect_callback_.Reset();
  run_callback_.Reset();
}

void NaiveClientConnection::DoCallback(int result) {
  DCHECK_NE(result, ERR_IO_PENDING);
  DCHECK(connect_callback_);

  // Since Run() may result in Read being called,
  // clear connect_callback_ up front.
  base::ResetAndReturn(&connect_callback_).Run(result);
}

void NaiveClientConnection::OnIOComplete(int result) {
  DCHECK_NE(next_state_, STATE_NONE);
  int rv = DoLoop(result);
  if (rv != ERR_IO_PENDING) {
    DoCallback(rv);
  }
}

int NaiveClientConnection::DoLoop(int last_io_result) {
  DCHECK_NE(next_state_, STATE_NONE);
  int rv = last_io_result;
  do {
    State state = next_state_;
    next_state_ = STATE_NONE;
    switch (state) {
      case STATE_CONNECT_CLIENT:
        DCHECK_EQ(rv, OK);
        rv = DoConnectClient();
        break;
      case STATE_CONNECT_CLIENT_COMPLETE:
        rv = DoConnectClientComplete(rv);
        break;
      case STATE_CONNECT_SERVER:
        DCHECK_EQ(rv, OK);
        rv = DoConnectServer();
      case STATE_CONNECT_SERVER_COMPLETE:
        rv = DoConnectServerComplete(rv);
        break;
      default:
        NOTREACHED() << "bad state";
        rv = ERR_UNEXPECTED;
        break;
    }
  } while (rv != ERR_IO_PENDING && next_state_ != STATE_NONE);
  return rv;
}

int NaiveClientConnection::DoConnectClient() {
  next_state_ = STATE_CONNECT_CLIENT_COMPLETE;

  return client_socket_->Connect(&request_endpoint_, io_callback_);
}

int NaiveClientConnection::DoConnectClientComplete(int result) {
  if (result < 0)
    return result;

  early_pull_pending_ = true;
  Pull(kClient, kServer);
  if (early_pull_result_ != ERR_IO_PENDING) {
    // Pull has completed synchronously.
    if (early_pull_result_ <= 0) {
      return early_pull_result_ ? early_pull_result_ : ERR_CONNECTION_CLOSED;
    }
  }

  next_state_ = STATE_CONNECT_SERVER;
  return OK;
}

int NaiveClientConnection::DoConnectServer() {
  ProxyInfo proxy_info;
  const ProxyList& proxy_list =
      session_->proxy_service()->config().proxy_rules().single_proxies;
  if (proxy_list.IsEmpty())
    return ERR_MANDATORY_PROXY_CONFIGURATION_FAILED;
  proxy_info.UseProxyList(proxy_list);

  HttpRequestInfo req_info;
  SSLConfig server_ssl_config;
  SSLConfig proxy_ssl_config;
  session_->GetSSLConfig(req_info, &server_ssl_config, &proxy_ssl_config);
  proxy_ssl_config.rev_checking_enabled = false;

  next_state_ = STATE_CONNECT_SERVER_COMPLETE;

  DCHECK_NE(request_endpoint_.port(), 0);

  LOG(INFO) << "Connection " << id_ << " to " << request_endpoint_.ToString();

  return InitSocketHandleForRawConnect(
      request_endpoint_, session_, proxy_info, server_ssl_config,
      proxy_ssl_config, PRIVACY_MODE_DISABLED, net_log_,
      server_socket_handle_.get(), io_callback_);
}

int NaiveClientConnection::DoConnectServerComplete(int result) {
  if (result < 0)
    return result;

  DCHECK(server_socket_handle_->socket());
  sockets_[kServer] = server_socket_handle_->socket();

  full_duplex_ = true;
  next_state_ = STATE_NONE;
  return OK;
}

int NaiveClientConnection::Run(const CompletionCallback& callback) {
  DCHECK(sockets_[kClient]);
  DCHECK(sockets_[kServer]);
  DCHECK_EQ(next_state_, STATE_NONE);
  DCHECK(!connect_callback_);

  if (errors_[kClient] != OK)
    return errors_[kClient];
  if (errors_[kServer] != OK)
    return errors_[kServer];

  run_callback_ = callback;

  bytes_passed_without_yielding_[kClient] = 0;
  bytes_passed_without_yielding_[kServer] = 0;

  yield_after_time_[kClient] =
      time_func_() +
      base::TimeDelta::FromMilliseconds(kYieldAfterDurationMilliseconds);
  yield_after_time_[kServer] = yield_after_time_[kClient];

  can_push_to_server_ = true;
  if (!early_pull_pending_) {
    DCHECK_GT(early_pull_result_, 0);
    Push(kClient, kServer, early_pull_result_);
  }
  Pull(kServer, kClient);

  return ERR_IO_PENDING;
}

void NaiveClientConnection::Pull(Direction from, Direction to) {
  if (errors_[kClient] < 0 || errors_[kServer] < 0)
    return;

  read_buffers_[from] = new IOBuffer(kBufferSize);
  DCHECK(sockets_[from]);
  int rv = sockets_[from]->Read(
      read_buffers_[from].get(), kBufferSize,
      base::Bind(&NaiveClientConnection::OnPullComplete,
                 weak_ptr_factory_.GetWeakPtr(), from, to));

  if (from == kClient && early_pull_pending_)
    early_pull_result_ = rv;

  if (rv != ERR_IO_PENDING)
    OnPullComplete(from, to, rv);
}

void NaiveClientConnection::Push(Direction from, Direction to, int size) {
  write_buffers_[to] = new DrainableIOBuffer(read_buffers_[from].get(), size);
  write_pending_[to] = true;
  DCHECK(sockets_[to]);
  int rv =
      sockets_[to]->Write(write_buffers_[to].get(), size,
                          base::Bind(&NaiveClientConnection::OnPushComplete,
                                     weak_ptr_factory_.GetWeakPtr(), from, to));

  if (rv != ERR_IO_PENDING)
    OnPushComplete(from, to, rv);
}

void NaiveClientConnection::Disconnect(Direction side) {
  if (sockets_[side]) {
    sockets_[side]->Disconnect();
    sockets_[side] = nullptr;
    write_pending_[side] = false;
  }
}

bool NaiveClientConnection::IsConnected(Direction side) {
  return sockets_[side];
}

void NaiveClientConnection::OnBothDisconnected() {
  if (run_callback_) {
    int error = OK;
    if (errors_[kClient] != ERR_CONNECTION_CLOSED && errors_[kClient] < 0)
      error = errors_[kClient];
    if (errors_[kServer] != ERR_CONNECTION_CLOSED && errors_[kClient] < 0)
      error = errors_[kServer];
    base::ResetAndReturn(&run_callback_).Run(error);
  }
}

void NaiveClientConnection::OnPullError(Direction from,
                                        Direction to,
                                        int error) {
  DCHECK_LT(error, 0);

  errors_[from] = error;
  Disconnect(from);

  if (!write_pending_[to])
    Disconnect(to);

  if (!IsConnected(from) && !IsConnected(to))
    OnBothDisconnected();
}

void NaiveClientConnection::OnPushError(Direction from,
                                        Direction to,
                                        int error) {
  DCHECK_LE(error, 0);
  DCHECK(!write_pending_[to]);

  if (error < 0) {
    errors_[to] = error;
    Disconnect(kServer);
    Disconnect(kClient);
  } else if (!IsConnected(from)) {
    Disconnect(to);
  }

  if (!IsConnected(from) && !IsConnected(to))
    OnBothDisconnected();
}

void NaiveClientConnection::OnPullComplete(Direction from,
                                           Direction to,
                                           int result) {
  if (from == kClient && early_pull_pending_) {
    early_pull_pending_ = false;
    early_pull_result_ = result;
  }

  if (result <= 0) {
    OnPullError(from, to, result ? result : ERR_CONNECTION_CLOSED);
    return;
  }

  if (from == kClient && !can_push_to_server_)
    return;

  Push(from, to, result);
}

void NaiveClientConnection::OnPushComplete(Direction from,
                                           Direction to,
                                           int result) {
  if (result >= 0) {
    bytes_passed_without_yielding_[from] += result;
    write_buffers_[to]->DidConsume(result);
    int size = write_buffers_[to]->BytesRemaining();
    if (size > 0) {
      Push(from, to, size);
      return;
    }
  }

  write_pending_[to] = false;
  // Checks for termination even if result is OK.
  OnPushError(from, to, result >= 0 ? OK : result);

  if (bytes_passed_without_yielding_[from] > kYieldAfterBytesRead ||
      time_func_() > yield_after_time_[from]) {
    bytes_passed_without_yielding_[from] = 0;
    yield_after_time_[from] =
        time_func_() +
        base::TimeDelta::FromMilliseconds(kYieldAfterDurationMilliseconds);
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::Bind(&NaiveClientConnection::Pull,
                              weak_ptr_factory_.GetWeakPtr(), from, to));
  } else {
    Pull(from, to);
  }
}

}  // namespace net
