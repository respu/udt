#ifndef UDT_IP_UDT_QUERY_H_
#define UDT_IP_UDT_QUERY_H_

#include <cstdint>

#include <string>

#include <boost/asio/ip/resolver_query_base.hpp>

#include "udt/connected_protocol/resolver_query.h"
#include "udt/connected_protocol/protocol.h"

namespace ip {

template <class UDTProtocol>
class UDTQuery : public connected_protocol::ResolverQuery<UDTProtocol> {
 public:
  typedef UDTProtocol protocol_type;
  typedef typename protocol_type::next_layer_protocol udp_protocol;

  typedef uint32_t SocketId;

  UDTQuery(const std::string& service,
           boost::asio::ip::resolver_query_base::flags resolve_flags =
               boost::asio::ip::resolver_query_base::flags::passive |
               boost::asio::ip::resolver_query_base::flags::address_configured,
           SocketId socket_id = 0)
      : connected_protocol::ResolverQuery<UDTProtocol>(
            udp_protocol::resolver::query(service, resolve_flags), socket_id) {}

  UDTQuery(const udp_protocol& protocol, const std::string& service,
           boost::asio::ip::resolver_query_base::flags resolve_flags =
               boost::asio::ip::resolver_query_base::flags::passive |
               boost::asio::ip::resolver_query_base::flags::address_configured,
           SocketId socket_id = 0)
      : connected_protocol::ResolverQuery<UDTProtocol>(
            typename udp_protocol::resolver::query(protocol, service,
                                                   resolve_flags),
            socket_id) {}

  UDTQuery(const std::string& host, const std::string& service,
           boost::asio::ip::resolver_query_base::flags resolve_flags =
               boost::asio::ip::resolver_query_base::flags::address_configured,
           SocketId socket_id = 0)
      : connected_protocol::ResolverQuery<UDTProtocol>(
            typename udp_protocol::resolver::query(host, service,
                                                   resolve_flags),
            socket_id) {}

  UDTQuery(const udp_protocol& protocol, const std::string& host,
           const std::string& service,
           boost::asio::ip::resolver_query_base::flags resolve_flags =
               boost::asio::ip::resolver_query_base::flags::address_configured,
           SocketId socket_id = 0)
      : connected_protocol::ResolverQuery<UDTProtocol>(
            typename udp_protocol::resolver::query(protocol, host,
                                                   resolve_flags),
            socket_id) {}
};

}  // ip

#endif  // UDT_IP_UDT_QUERY_H_
