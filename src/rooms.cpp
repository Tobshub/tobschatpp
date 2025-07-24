#include <hv/WebSocketChannel.h>
#include <map>
#include <string>

using namespace std;

class Room {
public:
  string name;
  map<int, const WebSocketChannelPtr &> members;
  Room(string name) {}

  void join(const WebSocketChannelPtr &channel) {
    members.insert({channel->id(), channel});
  }

  void leave(const WebSocketChannelPtr &channel) {
    members.erase(channel->id());
  }

  void broadcast(string message) {
    for (auto &member : members) {
      member.second->send(message);
    }
  }
};
