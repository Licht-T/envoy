#pragma once
#include "source/common/common/logger.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace MySQLProxy {

class MySQLSession : Logger::Loggable<Logger::Id::filter> {
public:
  enum class State {
    Init = 0,
    ChallengeReq = 1,
    ChallengeResp41 = 2,
    ChallengeResp320 = 3,
    SslPt = 4,
    AuthSwitchReq = 5,
    AuthSwitchReqOld = 6,
    AuthSwitchResp = 7,
    AuthSwitchMore = 8,
    ReqResp = 9,
    Req = 10,
    Resync = 11,
    NotHandled = 12,
    Error = 13,
  };

  void setState(MySQLSession::State state) { state_ = state; }
  MySQLSession::State getState() { return state_; }
  uint8_t getExpectedSeq(bool is_upstream) { return seq_ - (is_upstream ? downstream_drained_ : upstream_drained_); }
  uint8_t getExpectedSeqForReciever(bool is_upstream) { return seq_ - (is_upstream ? upstream_drained_ : downstream_drained_); }
  uint8_t convertToSeqOnReciever(uint8_t seq, bool is_upstream) { return seq + (is_upstream ? 1 : -1) * (downstream_drained_ - upstream_drained_); }
  void resetSeq() { seq_ = MYSQL_REQUEST_PKT_NUM; upstream_drained_ = 0; downstream_drained_ = 0; }
  void incSeq() { seq_++; }
  void incUpstreamDrained() { upstream_drained_++; }
  void incDownstreamDrained() { downstream_drained_++; }

private:
  MySQLSession::State state_{State::Init};
  uint8_t seq_{0};
  uint8_t upstream_drained_{0};
  uint8_t downstream_drained_{0};
};

} // namespace MySQLProxy
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
