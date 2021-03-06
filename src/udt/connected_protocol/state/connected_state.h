#ifndef UDT_CONNECTED_PROTOCOL_STATE_CONNECTED_STATE_H_
#define UDT_CONNECTED_PROTOCOL_STATE_CONNECTED_STATE_H_

#include <cstdint>

#include <memory>

#include <boost/bind.hpp>
#include <boost/chrono.hpp>
#include <boost/log/trivial.hpp>

#include "udt/connected_protocol/io/read_op.h"
#include "udt/connected_protocol/io/write_op.h"
#include "udt/connected_protocol/io/buffers.h"

#include "udt/connected_protocol/state/base_state.h"

#include "udt/connected_protocol/state/connected/sender.h"
#include "udt/connected_protocol/state/connected/receiver.h"

namespace connected_protocol {
namespace state {

template <class Protocol, class ConnectionPolicy>
class ConnectedState : public BaseState<Protocol>,
                       public std::enable_shared_from_this<
                           ConnectedState<Protocol, ConnectionPolicy>>,
                       public ConnectionPolicy {
 public:
  typedef std::shared_ptr<ConnectedState> Ptr;

 private:
  typedef typename Protocol::congestion_control CongestionControl;
  typedef typename Protocol::socket_session SocketSession;
  typedef typename Protocol::clock Clock;
  typedef typename Protocol::timer Timer;
  typedef typename Protocol::time_point TimePoint;
  typedef typename Protocol::logger Logger;

 private:
  typedef typename Protocol::SendDatagram SendDatagram;
  typedef std::shared_ptr<SendDatagram> SendDatagramPtr;
  typedef typename Protocol::DataDatagram DataDatagram;
  typedef std::shared_ptr<DataDatagram> DataDatagramPtr;
  typedef typename Protocol::ConnectionDatagram ConnectionDatagram;
  typedef std::shared_ptr<ConnectionDatagram> ConnectionDatagramPtr;
  typedef typename Protocol::GenericControlDatagram ControlDatagram;
  typedef std::shared_ptr<ControlDatagram> ControlDatagramPtr;
  typedef typename Protocol::AckDatagram AckDatagram;
  typedef std::shared_ptr<AckDatagram> AckDatagramPtr;
  typedef typename Protocol::NAckDatagram NAckDatagram;
  typedef std::shared_ptr<NAckDatagram> NAckDatagramPtr;
  typedef typename Protocol::AckOfAckDatagram AckOfAckDatagram;
  typedef std::shared_ptr<AckOfAckDatagram> AckOfAckDatagramPtr;
  typedef typename Protocol::KeepAliveDatagram KeepAliveDatagram;
  typedef std::shared_ptr<KeepAliveDatagram> KeepAliveDatagramPtr;
  typedef typename Protocol::ShutdownDatagram ShutdownDatagram;
  typedef std::shared_ptr<ShutdownDatagram> ShutdownDatagramPtr;

 private:
  typedef typename state::ClosedState<Protocol> ClosedState;
  typedef typename connected::Sender<Protocol, ConnectedState> Sender;
  typedef typename connected::Receiver<Protocol> Receiver;

 private:
  typedef uint32_t PacketSequenceNumber;
  typedef uint32_t AckSequenceNumber;

 public:
  static Ptr Create(typename SocketSession::Ptr p_session) {
    return Ptr(new ConnectedState(std::move(p_session)));
  }

  virtual ~ConnectedState() { StopServices(); }

  virtual typename BaseState<Protocol>::type GetType() {
    return this->CONNECTED;
  }

  virtual boost::asio::io_service& get_io_service() {
    return p_session_->get_io_service();
  }

  virtual void Init() {
    receiver_.Init(p_session_->init_packet_seq_num);
    sender_.Init(this->shared_from_this(), &congestion_control_);
    congestion_control_.Init(p_session_->init_packet_seq_num,
                             p_session_->max_window_flow_size);

    ack_timer_.expires_from_now(p_session_->connection_info.ack_period());
    ack_timer_.async_wait(boost::bind(&ConnectedState::AckTimerHandler,
                                      this->shared_from_this(), _1, false));
    exp_timer_.expires_from_now(p_session_->connection_info.exp_period());
    exp_timer_.async_wait(boost::bind(&ConnectedState::ExpTimerHandler,
                                      this->shared_from_this(), _1));

    /* Nack not send periodically anymore
    receiver_.nack_timer.expires_from_now(
        p_session_->connection_info.nack_period());
    receiver_.nack_timer.async_wait(boost::bind(
        &ConnectedState::NAckTimerHandler, this->shared_from_this(), _1));*/
  }

  virtual void Stop() {
    StopTimers();
    StopServices();
    CloseConnection();
  }

  virtual void Close() {
    p_session_->ChangeState(ClosedState::Create(p_session_->get_io_service()));
  }

  virtual void OnDataDgr(DataDatagram* p_datagram) {
    ResetExp(false);

    if (Logger::ACTIVE) {
      received_count_ = received_count_.load() + 1;
    }

    congestion_control_.OnPacketReceived(*p_datagram);
    receiver_.OnDataDatagram(p_datagram);

    packet_received_since_light_ack_ =
        packet_received_since_light_ack_.load() + 1;
    if (packet_received_since_light_ack_.load() >= 64) {
      AckTimerHandler(boost::system::error_code(), true);
    }
  }

  virtual void PushReadOp(
      io::basic_pending_stream_read_operation<Protocol>* read_op) {
    receiver_.PushReadOp(read_op);
  }

  virtual void PushWriteOp(io::basic_pending_write_operation* write_op) {
    sender_.PushWriteOp(write_op);
  }

  virtual bool HasPacketToSend() { return sender_.HasPacketToSend(); }

  virtual boost::chrono::nanoseconds NextScheduledPacketTime() {
    return sender_.NextScheduledPacketTime();
  }

  virtual SendDatagram* NextScheduledPacket() {
    SendDatagram* p_datagram(sender_.NextScheduledPacket());
    if (p_datagram) {
      congestion_control_.OnPacketSent(*p_datagram);
    }

    return p_datagram;
  }

  virtual void OnConnectionDgr(ConnectionDatagramPtr p_connection_dgr) {
    // Call policy to process connection datagram
    this->ProcessConnectionDgr(p_session_, std::move(p_connection_dgr));
  }

  virtual void OnControlDgr(ControlDatagram* p_control_dgr) {
    switch (p_control_dgr->header().flags()) {
      case ControlDatagram::Header::KEEP_ALIVE:
        ResetExp(false);
        break;
      case ControlDatagram::Header::ACK: {
        ResetExp(true);
        AckDatagram ack_dgr;
        boost::asio::buffer_copy(ack_dgr.GetMutableBuffers(),
                                 p_control_dgr->GetConstBuffers());
        ack_dgr.payload().set_payload_size(p_control_dgr->payload().GetSize());
        OnAck(ack_dgr);
        break;
      }
      case ControlDatagram::Header::NACK: {
        ResetExp(true);
        NAckDatagram nack_dgr;
        nack_dgr.payload().SetSize(p_control_dgr->payload().GetSize());
        boost::asio::buffer_copy(nack_dgr.GetMutableBuffers(),
                                 p_control_dgr->GetConstBuffers());
        OnNAck(nack_dgr);
        break;
      }
      case ControlDatagram::Header::SHUTDOWN:
        ResetExp(false);
        Close();
        break;
      case ControlDatagram::Header::ACK_OF_ACK: {
        ResetExp(false);
        AckOfAckDatagram ack_of_ack_dgr;
        boost::asio::buffer_copy(ack_of_ack_dgr.GetMutableBuffers(),
                                 p_control_dgr->GetConstBuffers());
        OnAckOfAck(ack_of_ack_dgr);
        break;
      }
      case ControlDatagram::Header::MESSAGE_DROP_REQUEST:
        ResetExp(false);
        break;
    }
  }

 private:
  ConnectedState(typename SocketSession::Ptr p_session)
      : p_session_(std::move(p_session)),
        sender_(p_session_->get_io_service(), p_session_),
        receiver_(p_session_->get_io_service(), p_session_),
        unqueue_write_op_(false),
        congestion_control_(&(p_session_->connection_info)),
        stop_timers_(false),
        ack_timer_(p_session_->get_timer_io_service()),
        nack_timer_(p_session_->get_timer_io_service()),
        exp_timer_(p_session_->get_timer_io_service()) {}

 private:
  void StopServices() {
    sender_.Stop();
    receiver_.Stop();
  }

  // Timer processing
 private:
  void StopTimers() {
    boost::system::error_code ec;
    stop_timers_ = true;
    ack_timer_.cancel(ec);
    nack_timer_.cancel(ec);
    exp_timer_.cancel(ec);
  }

  void AckTimerHandler(const boost::system::error_code& ec,
                       bool light_ack = false) {
    if (stop_timers_.load()) {
      return;
    }

    if (!light_ack) {
      LaunchAckTimer();
    }

    auto& packet_seq_gen = p_session_->packet_seq_gen;

    PacketSequenceNumber ack_number = receiver_.AckNumber(packet_seq_gen);

    if (!light_ack &&
        (ack_number == receiver_.largest_ack_number_acknowledged() ||
         ((ack_number == receiver_.last_ack_number()) &&
          boost::chrono::duration_cast<boost::chrono::microseconds>(
              Clock::now() - receiver_.last_ack_timestamp()) <
              2 * p_session_->connection_info.rtt()))) {
      return;
    }

    if (Logger::ACTIVE) {
      ack_sent_count_ = ack_sent_count_.load() + 1;
    }

    auto& ack_seq_gen = p_session_->ack_seq_gen;
    AckDatagramPtr p_ack_datagram = std::make_shared<AckDatagram>();
    auto& header = p_ack_datagram->header();
    auto& payload = p_ack_datagram->payload();
    AckSequenceNumber ack_seq_num = ack_seq_gen.current();
    ack_seq_gen.Next();

    payload.set_max_packet_sequence_number(ack_number);
    if (light_ack && packet_received_since_light_ack_.load() >= 64) {
      packet_received_since_light_ack_ = 0;
      payload.SetAsLightAck();
    } else {
      payload.SetAsFullAck();
      payload.set_rtt(
          static_cast<uint32_t>(p_session_->connection_info.rtt().count()));
      payload.set_rtt_var(
          static_cast<uint32_t>(p_session_->connection_info.rtt_var().count()));
      uint32_t available_buffer(receiver_.AvailableReceiveBufferSize());

      if (available_buffer < 2) {
        available_buffer = 2;
      }

      payload.set_available_buffer_size(available_buffer);

      payload.set_packet_arrival_speed(
          (uint32_t)ceil(receiver_.GetPacketArrivalSpeed()));
      payload.set_estimated_link_capacity(
          (uint32_t)ceil(receiver_.GetEstimatedLinkCapacity()));
    }

    // register ack
    receiver_.StoreAck(ack_seq_num, ack_number, light_ack);
    receiver_.set_last_ack_number(ack_number);

    header.set_timestamp((uint32_t)(
        boost::chrono::duration_cast<boost::chrono::microseconds>(
            receiver_.last_ack_timestamp() - p_session_->start_timestamp)
            .count()));

    auto self = this->shared_from_this();

    p_session_->AsyncSendControlPacket(
        *p_ack_datagram, AckDatagram::Header::ACK, ack_seq_num,
        [self, p_ack_datagram](const boost::system::error_code&, std::size_t) {
        });
  }

  void LaunchAckTimer() {
    if (stop_timers_.load()) {
      return;
    }
    ack_timer_.expires_from_now(p_session_->connection_info.ack_period());
    ack_timer_.async_wait(boost::bind(&ConnectedState::AckTimerHandler,
                                      this->shared_from_this(), _1, false));
  }

  virtual void Log(connected_protocol::logger::LogEntry* p_log) {
    p_log->received_count = received_count_.load();
    p_log->nack_count = nack_count_.load();
    p_log->ack_count = ack_count_.load();
    p_log->ack2_count = ack2_count_.load();
    p_log->local_arrival_speed = receiver_.GetPacketArrivalSpeed();
    p_log->local_estimated_link_capacity = receiver_.GetEstimatedLinkCapacity();
    p_log->ack_sent_count = ack_sent_count_.load();
    p_log->ack2_sent_count = ack2_sent_count_.load();
  }

  void ResetLog() {
    nack_count_ = 0;
    ack_count_ = 0;
    ack2_count_ = 0;
    received_count_ = 0;
    ack_sent_count_ = 0;
    ack2_sent_count_ = 0;
  }

  virtual double PacketArrivalSpeed() {
    return receiver_.GetPacketArrivalSpeed();
  }

  virtual double EstimatedLinkCapacity() {
    return receiver_.GetEstimatedLinkCapacity();
  }

  void NAckTimerHandler(const boost::system::error_code& ec) {
    if (stop_timers_.load()) {
      return;
    }
    nack_timer_.expires_from_now(p_session_->connection_info.nack_period());
    nack_timer_.async_wait(boost::bind(&ConnectedState::NAckTimerHandler,
                                       this->shared_from_this(), _1));
  }

  /// Reset expiration
  /// @param with_timer reset the timer as well
  void ResetExp(bool with_timer) {
    receiver_.ResetExpCounter();

    if (with_timer || !sender_.HasNackPackets()) {
      boost::system::error_code ec;
      exp_timer_.cancel(ec);
    }
  }

  void LaunchExpTimer() {
    if (stop_timers_.load()) {
      return;
    }

    p_session_->connection_info.UpdateExpPeriod(receiver_.exp_count());

    exp_timer_.expires_from_now(p_session_->connection_info.exp_period());
    exp_timer_.async_wait(boost::bind(&ConnectedState::ExpTimerHandler,
                                      this->shared_from_this(), _1));
  }

  void ExpTimerHandler(const boost::system::error_code& ec) {
    if (stop_timers_.load()) {
      return;
    }

    if (ec) {
      LaunchExpTimer();
      return;
    }

    if (!sender_.HasLossPackets()) {
      sender_.UpdateLossListFromNackPackets();
    }

    // session expired -> exp count > 16 && 10 seconds since last reset exp
    // counter
    if (receiver_.HasTimeout()) {
      BOOST_LOG_TRIVIAL(trace) << "Connected state : timeout";
      congestion_control_.OnTimeout();
      Close();
      return;
    }

    if (!sender_.HasLossPackets()) {
      // send keep alive datagram
      auto self = this->shared_from_this();
      KeepAliveDatagramPtr p_keep_alive_dgr =
          std::make_shared<KeepAliveDatagram>();

      p_session_->AsyncSendControlPacket(
          *p_keep_alive_dgr, KeepAliveDatagram::Header::KEEP_ALIVE,
          KeepAliveDatagram::Header::NO_ADDITIONAL_INFO,
          [self, p_keep_alive_dgr](const boost::system::error_code&,
                                   std::size_t) {});
    }

    receiver_.IncExpCounter();

    LaunchExpTimer();
  }

  // Packet processing
 private:
  void OnAck(const AckDatagram& ack_dgr) {
    auto self = this->shared_from_this();
    auto& packet_seq_gen = p_session_->packet_seq_gen;
    auto& header = ack_dgr.header();
    auto& payload = ack_dgr.payload();
    PacketSequenceNumber packet_ack_number =
        GetPacketSequenceValue(payload.max_packet_sequence_number());
    AckSequenceNumber ack_seq_num = header.additional_info();

    if (Logger::ACTIVE) {
      ack_count_ = ack_count_.load() + 1;
    }

    sender_.AckPackets(packet_ack_number);

    receiver_.set_last_ack2_seq_number(ack_seq_num);
    AckOfAckDatagramPtr p_ack2_dgr = std::make_shared<AckOfAckDatagram>();
    p_session_->AsyncSendControlPacket(
        *p_ack2_dgr, AckOfAckDatagram::Header::ACK_OF_ACK, ack_seq_num,
        [self, p_ack2_dgr](const boost::system::error_code&, std::size_t) {});

    if (payload.IsLightAck()) {
      if (packet_seq_gen.Compare(packet_ack_number,
                                 receiver_.largest_acknowledged_seq_number()) >=
          0) {
        // available buffer size in packets
        int32_t offset = packet_seq_gen.SeqOffset(
            receiver_.largest_acknowledged_seq_number(), packet_ack_number);
        p_session_->window_flow_size =
            p_session_->window_flow_size.load() - offset;
        receiver_.set_largest_acknowledged_seq_number(packet_ack_number);
      }
      return;
    }

    p_session_->connection_info.UpdateRTT(payload.rtt());
    uint32_t rtt_var = (uint32_t)abs((long long)payload.rtt() -
                                     p_session_->connection_info.rtt().count());
    p_session_->connection_info.UpdateRTTVar(rtt_var);
    p_session_->connection_info.UpdateAckPeriod();
    p_session_->connection_info.UpdateNAckPeriod();

    congestion_control_.OnAck(ack_dgr, packet_seq_gen);

    if (payload.IsFull()) {
      uint32_t arrival_speed = payload.packet_arrival_speed();
      uint32_t estimated_link = payload.estimated_link_capacity();
      if (arrival_speed > 0) {
        p_session_->connection_info.UpdatePacketArrivalSpeed(
            (double)payload.packet_arrival_speed());
      }
      if (estimated_link > 0) {
        p_session_->connection_info.UpdateEstimatedLinkCapacity(
            (double)payload.estimated_link_capacity());
      }
    }

    if (packet_seq_gen.Compare(packet_ack_number,
                               receiver_.largest_acknowledged_seq_number()) >=
        0) {
      receiver_.set_largest_acknowledged_seq_number(packet_ack_number);
      // available buffer size in packets
      p_session_->window_flow_size = payload.available_buffer_size();
    }
  }

  void OnNAck(const NAckDatagram& nack_dgr) {
    if (Logger::ACTIVE) {
      nack_count_ = nack_count_.load() + 1;
    }

    sender_.UpdateLossListFromNackDgr(nack_dgr);
    congestion_control_.OnLoss(nack_dgr, p_session_->packet_seq_gen);
  }

  void OnAckOfAck(const AckOfAckDatagram& ack_of_ack_dgr) {
    auto& packet_seq_gen = p_session_->packet_seq_gen;
    AckSequenceNumber ack_seq_num = ack_of_ack_dgr.header().additional_info();
    PacketSequenceNumber packet_seq_num(0);
    boost::chrono::microseconds rtt(0);
    bool acked = receiver_.AckAck(ack_seq_num, &packet_seq_num, &rtt);

    if (acked) {
      if (Logger::ACTIVE) {
        ack2_count_ = ack2_count_.load() + 1;
      }

      if (packet_seq_gen.Compare(packet_seq_num,
                                 receiver_.largest_ack_number_acknowledged()) >
          0) {
        receiver_.set_largest_ack_number_acknowledged(packet_seq_num);
      }

      p_session_->connection_info.UpdateRTT(rtt.count());
      uint64_t rtt_var =
          abs(p_session_->connection_info.rtt().count() - rtt.count());
      p_session_->connection_info.UpdateRTTVar(rtt_var);

      p_session_->connection_info.UpdateAckPeriod();
      p_session_->connection_info.UpdateNAckPeriod();
    }
  }

  void CloseConnection() {
    boost::system::error_code ec;
    congestion_control_.OnClose();
    auto self = this->shared_from_this();
    ShutdownDatagramPtr p_shutdown_dgr = std::make_shared<ShutdownDatagram>();
    auto shutdown_handler =
        [self, p_shutdown_dgr](const boost::system::error_code&, std::size_t) {
          self->p_session_->Unbind();
        };

    p_session_->p_connection_info_cache->Update(p_session_->connection_info);

    p_session_->AsyncSendControlPacket(
        *p_shutdown_dgr, ShutdownDatagram::Header::SHUTDOWN,
        ShutdownDatagram::Header::NO_ADDITIONAL_INFO, shutdown_handler);
  }

  PacketSequenceNumber GetPacketSequenceValue(
      PacketSequenceNumber seq_num) const {
    return seq_num & 0x7FFFFFFF;
  }

 private:
  typename SocketSession::Ptr p_session_;
  Sender sender_;
  Receiver receiver_;
  bool unqueue_write_op_;
  CongestionControl congestion_control_;
  std::atomic<bool> stop_timers_;
  Timer ack_timer_;
  Timer nack_timer_;
  Timer exp_timer_;
  std::atomic<uint32_t> nack_count_;
  std::atomic<uint32_t> ack_count_;
  std::atomic<uint32_t> ack_sent_count_;
  std::atomic<uint32_t> ack2_count_;
  std::atomic<uint32_t> ack2_sent_count_;
  std::atomic<uint32_t> received_count_;
  std::atomic<uint32_t> packet_received_since_light_ack_;
};

}  // state
}  // connected_protocol

#endif  // UDT_CONNECTED_PROTOCOL_STATE_CONNECTED_STATE_H_
