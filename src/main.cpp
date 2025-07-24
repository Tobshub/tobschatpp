#include "hv/HttpMessage.h"
#include "hv/hstring.h"
#include "message_reader.cpp"
#include "rooms.cpp"
#include <cstdio>
#include <hv/WebSocketChannel.h>
#include <hv/WebSocketServer.h>
#include <string>

enum Command {
  EXIT,
  NICKNAME,
};

static map<string, Command> commands = {
    {"/exit", EXIT},
    {"/nickname", NICKNAME},
};

static Room GLOBAL("global");

string handle_command(const WebSocketChannelPtr &channel, Command command,
                      MessageReader *reader) {
  switch (command) {
  case EXIT:
    channel->close();
  case NICKNAME:
    return "Set nickname: " + reader->read_to_end();
  }
  return "unhandled command";
}

string dispatch_message(const WebSocketChannelPtr &channel, string message) {
  cout << "recv: " << message << endl;
  MessageReader reader(message);
  string command_str = reader.read();
  if (command_str.substr(0, 1) != "/") {
    GLOBAL.broadcast(message);
    return "sent";
  }
  auto it = commands.find(command_str);
  if (it == commands.end()) {
    return "invalid command";
  }
  Command command = it->second;
  return handle_command(channel, command, &reader);
}

int main(int argc, char **argv) {
  HttpService http;
  http.GET("/",
           [](const HttpContextPtr &ctx) { return ctx->send("hello world!"); });

  WebSocketService ws;

  ws.onopen = [](const WebSocketChannelPtr &channel,
                 const HttpRequestPtr &req) {
    cout << "connected " << channel->id() << endl;
    GLOBAL.join(channel);
  };

  ws.onmessage = [](const WebSocketChannelPtr &channel,
                    const std::string &message) {
    string res = dispatch_message(channel, message);
    channel->send(res);
  };

  ws.onclose = [](const WebSocketChannelPtr &channel) {
    cout << "disconnected " << channel->id() << endl;
    GLOBAL.leave(channel);
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
