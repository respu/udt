#ifndef UDT_CONNECTED_PROTOCOL_CACHE_CONNECTIONS_INFO_MANAGER_H_
#define UDT_CONNECTED_PROTOCOL_CACHE_CONNECTIONS_INFO_MANAGER_H_

#include <cstdint>

#include <atomic>
#include <map>
#include <string>

#include <boost/thread/recursive_mutex.hpp>

#include "udt/connected_protocol/cache/connection_info.h"

namespace connected_protocol {
namespace cache {

template <class Protocol>
class ConnectionsInfoManager {
 public:
  typedef typename Protocol::next_layer_protocol::endpoint NextEndpoint;

 private:
  typedef std::string remote_address;
  typedef std::map<remote_address, ConnectionInfo::Ptr> ConnectionsInfoMap;

 public:
  typedef typename Protocol::endpoint Endpoint;

 public:
  ConnectionsInfoManager(uint32_t max_cache_size = 64)
      : max_cache_size_(max_cache_size),
        connections_mutex_(),
        connections_info_() {}

  ConnectionInfo::Ptr GetConnectionInfo(const NextEndpoint& next_endpoint) {
    boost::recursive_mutex::scoped_lock lock_connections(connections_mutex_);
    std::string address(next_endpoint.address().to_string());
    ConnectionsInfoMap::iterator connection_info_it(
        connections_info_.find(address));
    if (connection_info_it != connections_info_.end()) {
      return connection_info_it->second;
    }

    if (connections_info_.size() > max_cache_size_) {
      FreeItem();
    }

    ConnectionInfo::Ptr p_connection_info(std::make_shared<ConnectionInfo>());
    connections_info_[address] = p_connection_info;

    return p_connection_info;
  }

 private:
  void FreeItem() {
    boost::recursive_mutex::scoped_lock lock_connections(connections_mutex_);
    ConnectionsInfoMap::iterator oldest_pair_it(connections_info_.begin());
    ConnectionsInfoMap::iterator current_pair_it(oldest_pair_it);
    ConnectionsInfoMap::iterator end_pair_it(connections_info_.end());
    while (current_pair_it != end_pair_it) {
      if (current_pair_it->second < oldest_pair_it->second) {
        oldest_pair_it = current_pair_it;
      }
      ++current_pair_it;
    }

    connections_info_.erase(oldest_pair_it);
  }

 private:
  uint32_t max_cache_size_;
  boost::recursive_mutex connections_mutex_;
  ConnectionsInfoMap connections_info_;
};

}  // congestion
}  // connected_protocol

#endif  // UDT_CONNECTED_PROTOCOL_CACHE_CONNECTIONS_INFO_MANAGER_H_
