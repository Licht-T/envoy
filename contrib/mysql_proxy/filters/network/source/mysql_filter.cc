#include <algorithm>

#include "contrib/mysql_proxy/filters/network/source/mysql_filter.h"

#include "envoy/config/core/v3/base.pb.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/assert.h"
#include "source/common/common/logger.h"
#include "source/extensions/filters/network/well_known_names.h"

#include "contrib/mysql_proxy/filters/network/source/mysql_codec.h"
#include "contrib/mysql_proxy/filters/network/source/mysql_codec_clogin_resp.h"
#include "contrib/mysql_proxy/filters/network/source/mysql_decoder_impl.h"
#include "contrib/mysql_proxy/filters/network/source/mysql_utils.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace MySQLProxy {

MySQLFilterConfig::MySQLFilterConfig(const std::string& stat_prefix, Stats::Scope& scope, bool terminate_ssl)
    : scope_(scope),
      stats_(generateStats(stat_prefix, scope)),
      terminate_ssl_(terminate_ssl) {}

MySQLFilter::MySQLFilter(MySQLFilterConfigSharedPtr config) : config_(std::move(config)) {}

void MySQLFilter::initializeReadFilterCallbacks(Network::ReadFilterCallbacks& callbacks) {
  read_callbacks_ = &callbacks;
}

Network::FilterStatus MySQLFilter::onData(Buffer::Instance& data, bool) {
  Network::FilterStatus status = Network::FilterStatus::Continue;
  uint64_t remaining = read_buffer_.length();

  // Safety measure just to make sure that if we have a decoding error we keep going and lose stats.
  // This can be removed once we are more confident of this code.
  if (!sniffing_) {
    return status;
  }

  read_buffer_.add(data);
  status = doDecode(read_buffer_, true);

  if (status == Network::FilterStatus::StopIteration) {
    // FIXME: 必要なぶんだけDrainできないか検討
    data.drain(data.length());
    getSession().incUpstreamDrained();
    return status;
  }

  if (config_->terminate_ssl_) {
    doRewrite(data, remaining, true);
  }

  return status;
}

Network::FilterStatus MySQLFilter::onWrite(Buffer::Instance& data, bool) {
  Network::FilterStatus status = Network::FilterStatus::Continue;
  uint64_t remaining = write_buffer_.length();

  // Safety measure just to make sure that if we have a decoding error we keep going and lose stats.
  // This can be removed once we are more confident of this code.
  if (!sniffing_) {
    return status;
  }

  write_buffer_.add(data);
  status = doDecode(write_buffer_, false);

  if (status == Network::FilterStatus::StopIteration) {
    // FIXME: 必要なぶんだけDrainできないか検討
    data.drain(data.length());
    getSession().incDownstreamDrained();
    return status;
  }

  if (config_->terminate_ssl_) {
    doRewrite(data, remaining, false);
  }

  return status;
}

bool MySQLFilter::onSSLRequest() {
  if (!config_->terminate_ssl_) {
    return true;
  }

  if (!read_callbacks_->connection().startSecureTransport()) {
    ENVOY_CONN_LOG(info, "mysql_proxy: cannot enable secure transport. Check configuration.",
                   read_callbacks_->connection());
    read_callbacks_->connection().close(Network::ConnectionCloseType::NoFlush);
  } else {
    ENVOY_CONN_LOG(trace, "mysql_proxy: enabled SSL termination.",
                   read_callbacks_->connection());
  }

  return false;
}

Network::FilterStatus MySQLFilter::doDecode(Buffer::Instance& buffer, bool is_upstream) {
  // Clear dynamic metadata.
  envoy::config::core::v3::Metadata& dynamic_metadata =
      read_callbacks_->connection().streamInfo().dynamicMetadata();
  auto& metadata =
      (*dynamic_metadata.mutable_filter_metadata())[NetworkFilterNames::get().MySQLProxy];
  metadata.mutable_fields()->clear();

  if (!decoder_) {
    decoder_ = createDecoder(*this);
  }

  try {
    switch (decoder_->onData(buffer, is_upstream)) {
    case Decoder::Result::ReadyForNext:
      return Network::FilterStatus::Continue;
    case Decoder::Result::Stopped:
      return Network::FilterStatus::StopIteration;
    }
  } catch (EnvoyException& e) {
    ENVOY_LOG(info, "mysql_proxy: decoding error: {}", e.what());
    config_->stats_.decoder_errors_.inc();
    sniffing_ = false;
    read_buffer_.drain(read_buffer_.length());
    write_buffer_.drain(write_buffer_.length());
  }

  return Network::FilterStatus::Continue;
}

DecoderPtr MySQLFilter::createDecoder(DecoderCallbacks& callbacks) {
  return std::make_unique<DecoderImpl>(callbacks);
}

void MySQLFilter::doRewrite(Buffer::Instance& data, uint64_t remaining, bool is_upstream) {
  MySQLSession::State state = getSession().getState();
  auto& payload_metadata_list = decoder_->getPayloadMetadataList();
  uint64_t max_data_size = data.length();

  for (size_t i = 0; i < payload_metadata_list.size(); ++i) {
    uint8_t seq = payload_metadata_list[i].seq;
    uint32_t len = payload_metadata_list[i].len;

    if (i == 0 && remaining > 0) {
      len -= remaining - 4;
    } else {
      BufferHelper::consumeHdr(data);
      max_data_size -= 4;

      BufferHelper::addUint24(data, len);
      BufferHelper::addUint8(data, seq);

      if (is_upstream && (state == MySQLSession::State::ChallengeResp41 || state == MySQLSession::State::ChallengeResp320)){
        uint32_t client_cap = 0;
        BufferHelper::readUint32(data, client_cap);
        len -= 4;
        BufferHelper::addUint32(data, client_cap ^ (client_cap & CLIENT_SSL));
      }
    }

    std::string payload;
    uint64_t copy_size = std::min(static_cast<uint64_t>(len), max_data_size);
    payload.reserve(copy_size);
    BufferHelper::readStringBySize(data, copy_size, payload);
    BufferHelper::addBytes(data, payload.c_str(), payload.size());
    max_data_size -= copy_size;
  }
}

void MySQLFilter::onProtocolError() { config_->stats_.protocol_errors_.inc(); }

void MySQLFilter::onClientLogin(ClientLogin& client_login, MySQLSession::State state) {
  if (client_login.isSSLRequest()) {
    config_->stats_.upgraded_to_ssl_.inc();
  }

  if (state == MySQLSession::State::ChallengeResp41 || state == MySQLSession::State::ChallengeResp320) {
    config_->stats_.login_attempts_.inc();
  }
}

void MySQLFilter::onClientLoginResponse(ClientLoginResponse& client_login_resp) {
  if (client_login_resp.getRespCode() == MYSQL_RESP_AUTH_SWITCH) {
    config_->stats_.auth_switch_request_.inc();
  } else if (client_login_resp.getRespCode() == MYSQL_RESP_ERR) {
    config_->stats_.login_failures_.inc();
  }
}

void MySQLFilter::onMoreClientLoginResponse(ClientLoginResponse& client_login_resp) {
  if (client_login_resp.getRespCode() == MYSQL_RESP_ERR) {
    config_->stats_.login_failures_.inc();
  }
}

void MySQLFilter::onCommand(Command& command) {
  if (!command.isQuery()) {
    return;
  }

  // Parse a given query
  envoy::config::core::v3::Metadata& dynamic_metadata =
      read_callbacks_->connection().streamInfo().dynamicMetadata();
  ProtobufWkt::Struct metadata(
      (*dynamic_metadata.mutable_filter_metadata())[NetworkFilterNames::get().MySQLProxy]);

  auto result = Common::SQLUtils::SQLUtils::setMetadata(command.getData(),
                                                        decoder_->getAttributes(), metadata);

  ENVOY_CONN_LOG(trace, "mysql_proxy: query processed {}, result {}, cmd type {}",
                 read_callbacks_->connection(), command.getData(), result,
                 static_cast<int>(command.getCmd()));

  if (!result) {
    config_->stats_.queries_parse_error_.inc();
    return;
  }
  config_->stats_.queries_parsed_.inc();

  read_callbacks_->connection().streamInfo().setDynamicMetadata(
      NetworkFilterNames::get().MySQLProxy, metadata);
}

Network::FilterStatus MySQLFilter::onNewConnection() {
  config_->stats_.sessions_.inc();
  return Network::FilterStatus::Continue;
}

} // namespace MySQLProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
