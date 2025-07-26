#include "hv/HttpMessage.h"
#include "hv/hstring.h"
#include "message_reader.cpp"
#include "string_utils.hpp"
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

map<int, Context *> ACTIVE_CONTEXT = {};

class Context {
public:
  const WebSocketChannelPtr &channel;
  string nickname;
  Room *room;
  map<uuid, Room *> invites;
  map<uuid, Room *> rooms;
  explicit Context(const WebSocketChannelPtr &channel, Room *room)
      : channel(channel), room(room) {
    ACTIVE_CONTEXT.insert_or_assign(channel->id(), this);
  }
  virtual ~Context() { ACTIVE_CONTEXT.erase(channel->id()); };
  static Context *build(const WebSocketChannelPtr &channel,
                        Room *default_room) {
    auto it = ACTIVE_CONTEXT.find(channel->id());
    if (it != ACTIVE_CONTEXT.end()) {
      return it->second;
    }
    auto ctx = new Context(channel, default_room);
    ctx->join(default_room);
    return ctx;
  };
  int id() { return channel->id(); }
  void close() {
    channel->close();
    delete this;
  };
  void join(Room *room);
  void leave(Room *room);

  string nickOrId() {
    return this->nickname.empty() ? to_string(this->id()) : this->nickname;
  }
};

class Room {
public:
  uuid id;
  string name;
  map<int, Context *> members;
  Room(string name) : name(name) { this->id = uuid::random(); }
  void join(Context *ctx) {
    this->members.insert_or_assign(ctx->id(), ctx);
    this->broadcast(ctx,
                    std::format("@{} joined #{}", ctx->id(), this->nameOrId()));
  }

  void leave(Context *ctx) {
    members.erase(ctx->id());
    this->broadcast(ctx,
                    std::format("@{} left #{}", ctx->id(), this->nameOrId()));
  }

  string nameOrId() {
    return this->name.empty() ? this->id.string() : this->name;
  }

  void broadcast(Context *ctx, string message) {
    for (auto &member : members) {
      member.second->channel->send(
          format("#{}@{}: {}", this->nameOrId(), ctx->nickOrId(), message));
    }
  }
};

// call `room`.join and add room to context room list
void Context::join(Room *room) {
  room->join(this);
  this->rooms.insert_or_assign(room->id, room);
}

void Context::leave(Room *room) {
  room->leave(this);
  this->rooms.erase(room->id);
}

enum Command {
  EXIT,
  NICKNAME,
  INVITE,
  ACCEPT,
  LEAVE,
  ROOMS,
  ROOM,
  MESSAGE,
  MEMBERS,
};

static map<string, Command> commands = {
    {"/exit", EXIT},       {"/nickname", NICKNAME}, {"/invite", INVITE},
    {"/accept", ACCEPT},   {"/rooms", ROOMS},       {"/room", ROOM},
    {"/message", MESSAGE}, {"/leave", LEAVE},       {"/members", MEMBERS},
};

static Room GLOBAL("global");
static map<string, int> NICK_TO_ID;

string handle_command(Context *ctx, Command command, MessageReader *reader) {
  switch (command) {
  case EXIT:
    ctx->close();
  case NICKNAME: {
    string nickname = string_utils::trim(reader->read_to_end());
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
  case INVITE: {
    string idOrNick = trim(reader->read());
    int id;
    if (idOrNick[0] == '@') {
      auto it = NICK_TO_ID.find(idOrNick.substr(1));
      if (it == NICK_TO_ID.end()) {
        return "no such user";
      }
      id = it->second;
    } else {
      id = stoi(idOrNick);
    }
    auto recv = ACTIVE_CONTEXT.find(id);
    if (recv == ACTIVE_CONTEXT.end()) {
      return "no such user";
    }
    auto invite_ctx = recv->second;
    Room *new_room = new Room(ctx->nickOrId() + "," + invite_ctx->nickOrId());
    ctx->join(new_room);
    invite_ctx->invites.insert({new_room->id, new_room});
    invite_ctx->channel->send(std::format("invite from {} ({})", ctx->nickname,
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
    ctx->join(room);
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
    string message = string_utils::trim(reader->read_to_end());
    ctx->room->broadcast(ctx, message);
    return "sent";
  }
  case MEMBERS: {
    if (ctx->room == nullptr) {
      return "not in a room";
    }
    string members = "Members: ";
    for (auto it : ctx->room->members) {
      members += format("\n  @{} ({})", it.second->nickOrId(), it.second->id());
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
    cout << "ROOM: " << ctx->room << endl;
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
    string res = dispatch_message(Context::build(channel, &GLOBAL), message);
    channel->send(res);
  };

  ws.onclose = [](const WebSocketChannelPtr &channel) {
    println(format("disconnected: @{}", channel->id()));
    GLOBAL.leave(Context::build(channel, nullptr));
  };

  WebSocketServer server;
  server.port = 8080;

  server.registerHttpService(&http);
  server.registerWebSocketService(&ws);
  server.start();

  println("server started");

  while (getchar() != '\n')
    ;
  return 0;
}
