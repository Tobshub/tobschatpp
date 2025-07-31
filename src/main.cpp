#include "hv/HttpMessage.h"
#include "hv/hstring.h"
#include "message_reader.cpp"
#include <cstdio>
#include <format>
#include <hv/WebSocketChannel.h>
#include <hv/WebSocketServer.h>
#include <libuuidpp.hpp>
#include <string>

using namespace std;

typedef libuuidpp::uuid uuid;

class Room;
class Context;
class NoopChannel;

enum RoomPermission {
  Owner,
  Admin,
  Chat,
  Notify,

  None,
};

map<string, RoomPermission> string_perm_map{
    {"owner", Owner},   {"admin", Admin}, {"chat", Chat},
    {"notify", Notify}, {"none", None},
};

RoomPermission permission_from_string(string perm) {
  if (!string_perm_map.contains(perm))
    return None;
  return string_perm_map.at(perm);
}

map<int, Context *> ACTIVE_CONTEXT = {};

class Context {
public:
  const WebSocketChannelPtr &channel;
  string nickname;
  bool is_active;
  Room *room;
  map<uuid, Room *> invites;
  map<uuid, Room *> rooms;
  explicit Context(const WebSocketChannelPtr &channel, Room *room,
                   bool is_active = true)
      : channel(channel), room(room), is_active(is_active) {
    ACTIVE_CONTEXT.insert_or_assign(channel->id(), this);
  }
  virtual ~Context() {
    if (this->is_active)
      ACTIVE_CONTEXT.erase(channel->id());
  };
  static Context *build(const WebSocketChannelPtr &channel,
                        Room *default_room) {
    auto it = ACTIVE_CONTEXT.find(channel->id());
    if (it != ACTIVE_CONTEXT.end()) {
      return it->second;
    }
    auto ctx = new Context(channel, default_room);
    ctx->join(default_room, RoomPermission::Chat);
    return ctx;
  };
  int id() { return channel->id(); }
  void close();
  void join(Room *room, RoomPermission);
  void leave(Room *room);

  string nickOrId() {
    return this->nickname.empty() ? to_string(this->id()) : this->nickname;
  }

  void send(string message) {
    if (this->channel->isClosed()) {
      return;
    }
    this->channel->send(message);
  }
};

class NoopChannel : public WebSocketChannel {
public:
  NoopChannel() : WebSocketChannel(io()) {}
  int send(string message) {
    println("noop send >> " + message);
    return 0;
  }
  int id() { return -1; }
};

typedef struct RoomMember {
  Context *ctx;
  RoomPermission permission;
} RoomMember;

class Room {
public:
  uuid id;
  string name;
  Context *ctx;
  map<int, RoomMember> members;

  Room(string name) : name(name) {
    this->id = uuid::random();
    auto noop = new NoopChannel();
    this->ctx = new Context(WebSocketChannelPtr(noop), this, false);
    this->ctx->nickname = "internal";
  }

  void join(RoomMember member) {
    this->members.insert_or_assign(member.ctx->id(), member);
    this->broadcast(this->ctx, std::format("@{} joined #{}", member.ctx->id(),
                                           this->nameOrId()));
  }

  void leave(Context *ctx) {
    members.erase(ctx->id());
    this->broadcast(this->ctx,
                    std::format("@{} left #{}", ctx->id(), this->nameOrId()));
  }

  string nameOrId() {
    return this->name.empty() ? this->id.string() : this->name;
  }

  void broadcast(Context *ctx, string message) {
    if (ctx != this->ctx && !this->members.contains(ctx->id())) {
      return;
    }
    if (ctx != this->ctx) {
      auto sender = this->members.at(ctx->id());
      if (sender.permission > RoomPermission::Chat) {
        return;
      }
    }
    for (auto &member : members) {
      println(format("sending: {}", member.first));
      member.second.ctx->send(
          format("#{}@{}: {}", this->nameOrId(), ctx->nickOrId(), message));
    }
  }
};

void Context::close() {
  for (auto &room : this->rooms) {
    room.second->leave(this);
  }
  this->channel->close();
}

// call `room`.join and add room to context room list
void Context::join(Room *room, RoomPermission perm) {
  auto member = RoomMember{.ctx = this, .permission = perm};
  println("IN JOIN");
  room->join(member);
  this->rooms.insert_or_assign(room->id, room);
}

void Context::leave(Room *room) {
  room->leave(this);
  this->rooms.erase(room->id);
}

enum Command {
  EXIT,

  NICKNAME,

  ROOMS,

  ROOM,
  INVITE,
  ACCEPT,
  LEAVE,

  MEMBERS,
  RENAME,
  MESSAGE,

  PERMSET,
};

static map<string, Command> commands = {
    {"/exit", EXIT},       {"/nickname", NICKNAME}, {"/invite", INVITE},
    {"/accept", ACCEPT},   {"/rooms", ROOMS},       {"/room", ROOM},
    {"/message", MESSAGE}, {"/leave", LEAVE},       {"/members", MEMBERS},
    {"/rename", RENAME},   {"/permset", PERMSET},
};

static Room GLOBAL("global");
static map<string, int> NICK_TO_ID;

int parse_id_or_nick(string idOrNick) {
  int id;
  if (idOrNick[0] == '@') {
    auto it = NICK_TO_ID.find(idOrNick.substr(1));
    if (it == NICK_TO_ID.end()) {
      return -1;
    }
    id = it->second;
  } else {
    id = stoi(idOrNick);
  }
  return id;
}

string handle_command(Context *ctx, Command command, MessageReader *reader) {
  switch (command) {
  case EXIT:
    ctx->close();
    return "";
  case NICKNAME: {
    string nickname = trim(reader->read_to_end());
    if (nickname.empty()) {
      return ctx->nickname;
    }
    if (NICK_TO_ID.find(nickname) != NICK_TO_ID.end()) {
      return "nickname taken";
    }
    NICK_TO_ID.erase(ctx->nickname);
    ctx->nickname = nickname;
    NICK_TO_ID.insert({ctx->nickname, ctx->id()});
    return "Set nickname: " + nickname;
  }
  case ROOMS: {
    string rooms = "Rooms:";
    for (auto &it : ctx->rooms) {
      auto room = it.second;
      rooms += "\n  " + format("#{} ({})", room->name, room->id.string());
    }
    return rooms;
  }
  case ROOM: {
    string id = trim(reader->read());
    if (id.empty()) {
      if (ctx->room != nullptr) {
        return format("Current room: #{} ({})", ctx->room->name,
                      ctx->room->id.string());
      }
      return "not in room";
    }
    if (id[0] != '#') {
      return "invalid room id";
    }
    uuid room_id = uuid(id.substr(1));
    auto it = ctx->rooms.find(room_id);
    if (it == ctx->rooms.end()) {
      return "not in room";
    }
    auto room = it->second;
    ctx->room = room;
    return format("Changed default room: #{}", room->nameOrId());
  }
  case RENAME: {
    if (ctx->room->members.at(ctx->id()).permission > RoomPermission::Admin) {
      return "insufficient permissions";
    }
    string name = trim(reader->read_to_end());
    if (name.empty()) {
      return "invalid room name";
    }
    ctx->room->name = name;
    ctx->room->broadcast(ctx->room->ctx,
                         format("room name changed to {}", name));
    return "";
  }
  case PERMSET: {
    if (ctx->room->members.at(ctx->id()).permission > RoomPermission::Admin) {
      return "insufficient permissions";
    }
    int id = parse_id_or_nick(trim(reader->read()));
    if (id < 0) {
      return "no such user";
    }
    auto member = ctx->room->members.at(id);
    member.permission = permission_from_string(reader->read());
    ctx->room->members[id] = member;
  }
  case INVITE: {
    int id = parse_id_or_nick(trim(reader->read()));
    if (id < 0) {
      return "no such user";
    }
    auto recv = ACTIVE_CONTEXT.find(id);
    if (recv == ACTIVE_CONTEXT.end()) {
      return "no such user";
    }
    auto invite_ctx = recv->second;
    Room *new_room = new Room(ctx->nickOrId() + "," + invite_ctx->nickOrId());
    ctx->join(new_room, RoomPermission::Owner);
    println("AFTER JOIN");
    invite_ctx->invites.insert({new_room->id, new_room});
    invite_ctx->send(std::format("invite from {} ({})", ctx->nickOrId(),
                                 new_room->id.string()));
    return "invited";
  }
  case ACCEPT: {
    string id = trim(reader->read());
    auto invite = ctx->invites.find(uuid(id));
    if (invite == ctx->invites.end()) {
      return "no such invite";
    }
    auto room = invite->second;
    cout << "Accept invite Room: " << &room << endl;
    ctx->join(room, RoomPermission::Admin);
    ctx->invites.erase(invite->first);
    return "invite accepted: " + room->nameOrId();
  }
  case LEAVE: {
    string id = trim(reader->read());
    if (id.empty() || id[0] != '#') {
      return "invalid room id";
    }
    uuid room_id = uuid(id.substr(1));
    auto it = ctx->rooms.find(room_id);
    if (it == ctx->rooms.end()) {
      return "not in room";
    }
    auto room = it->second;
    ctx->leave(room);
    return "left";
  }
  case MESSAGE: {
    if (ctx->room == nullptr) {
      return "not in a room";
    }
    string message = trim(reader->read_to_end());
    ctx->room->broadcast(ctx, message);
    return "sent";
  }
  case MEMBERS: {
    if (ctx->room == nullptr) {
      return "not in a room";
    }
    string members = "Members: ";
    for (auto it : ctx->room->members) {
      members += format("\n  @{} ({})", it.second.ctx->nickOrId(),
                        it.second.ctx->id());
    }
    return members;
  }
  }
  return "unhandled command";
}

string dispatch_message(Context *ctx, string message) {
  // cout << "recv: " << message << endl;
  MessageReader reader(message);
  string command_str = reader.read();
  if (command_str.substr(0, 1) != "/") {
    if (ctx->room == nullptr) {
      return "not in a room";
    }
    ctx->room->broadcast(ctx, message);
    return "sent";
  }
  auto it = commands.find(command_str);
  if (it == commands.end()) {
    return "invalid command";
  }
  Command command = it->second;
  return handle_command(ctx, command, &reader);
}

int main(int argc, char **argv) {
  HttpService http;
  http.GET("/",
           [](const HttpContextPtr &ctx) { return ctx->send("hello world!"); });

  WebSocketService ws;

  ws.onopen = [](const WebSocketChannelPtr &channel,
                 const HttpRequestPtr &req) {
    println(format("connected: @{}", channel->id()));
    Context::build(channel, &GLOBAL);
  };

  ws.onmessage = [](const WebSocketChannelPtr &channel, const string &message) {
    auto ctx = Context::build(channel, &GLOBAL);
    string res = dispatch_message(ctx, message);
    if (!res.empty())
      ctx->send(res);
  };

  ws.onclose = [](const WebSocketChannelPtr &channel) {
    auto ctx = Context::build(channel, nullptr);
    println(format("disconnected: @{}", ctx->id()));
    ACTIVE_CONTEXT.erase(ctx->id());
    delete ctx;
  };

  WebSocketServer server;
  server.port = 8080;

  server.registerHttpService(&http);
  server.registerWebSocketService(&ws);
  server.start();

  println(format("server started :: {}", server.port));

  while (getchar() != '\n')
    ;
  return 0;
}
