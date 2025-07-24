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
    return new Context(channel, default_room);
  };
  int id() { return channel->id(); }
  void close() {
    channel->close();
    delete this;
  };
};

class Room {
public:
  uuid id;
  string name;
  map<int, Context *> members;
  Room(string name) : name(name) { this->id = uuid::random(); }
  void join(Context *ctx) {
    this->members.insert_or_assign(ctx->id(), ctx);
    cout << "Room::join: " << this << endl;
    println(std::format("{} joined #{}", ctx->id(), this->name));
  }

  void leave(Context *context) { members.erase(context->id()); }

  void broadcast(Context *ctx, string message) {
    println(this->name);
    for (auto &member : members) {
      string fmt_message =
          "#" + this->name + ":" + ctx->nickname + ": " + message;
      member.second->channel->send(fmt_message);
    }
  }
};

enum Command {
  EXIT,
  NICKNAME,
  INVITE,
  ACCEPT,
};

static map<string, Command> commands = {{"/exit", EXIT},
                                        {"/nickname", NICKNAME},
                                        {"/invite", INVITE},
                                        {"/accept", ACCEPT}};

static Room GLOBAL("global");

string handle_command(Context *ctx, Command command, MessageReader *reader) {
  switch (command) {
  case EXIT:
    ctx->close();
  case NICKNAME: {
    string nickname = string_utils::trim(reader->read_to_end());
    if (nickname.empty()) {
      return ctx->nickname;
    }
    ctx->nickname = nickname;
    return "Set nickname: " + nickname;
  }
  case INVITE: {
    int id = stoi(trim(reader->read()));
    auto recv = ACTIVE_CONTEXT.find(id);
    if (ACTIVE_CONTEXT.find(id) == ACTIVE_CONTEXT.end()) {
      return "no such user";
    }
    auto invite_ctx = recv->second;
    string room_name = ctx->nickname + ", " + invite_ctx->nickname;
    Room *new_room = new Room(room_name);
    new_room->join(ctx);
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
    room->join(ctx);
    ctx->invites.erase(invite->first);
    return "invite accepted: " + room->name;
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
    cout << "connected " << channel->id() << endl;
    GLOBAL.join(Context::build(channel, &GLOBAL));
  };

  ws.onmessage = [](const WebSocketChannelPtr &channel, const string &message) {
    string res = dispatch_message(Context::build(channel, &GLOBAL), message);
    channel->send(res);
  };

  ws.onclose = [](const WebSocketChannelPtr &channel) {
    cout << "disconnected " << channel->id() << endl;
    GLOBAL.leave(Context::build(channel, &GLOBAL));
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
