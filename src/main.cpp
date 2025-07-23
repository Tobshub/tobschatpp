#include "hv/HttpMessage.h"
#include "hv/hstring.h"
#include <cstdio>
#include <hv/WebSocketChannel.h>
#include <hv/WebSocketServer.h>
#include <sstream>
#include <string>

using namespace hv;
using namespace std;

class MessageReader {
public:
  string message;
  stringstream stream;
  vector<string> tokens;
  string token;

  MessageReader(string message) {
    // this->message = message;
    this->stream = stringstream(message);
  }

  string read() {
    if (stream.eof()) { return ""; }
    std::getline(stream, token, ' ');
    tokens.push_back(token);
    return token;
  }

  string read_to_end() {
    if (stream.eof()) { return ""; }
    return stream.str().substr(stream.tellg());
  }
};

enum Command {
  NICKNAME,
};

static map<string, Command> commands = {
    {"/nickname", NICKNAME},
};

string handle_command(Command command, MessageReader* reader) {
  switch (command) {
    case NICKNAME:
      return "Set nickname: " + reader->read_to_end();
  }
  return "unhandled command";
}

string dispatch_message(string message) {
  cout << "recv: " << message << endl;
  MessageReader reader(message);
  string command_str = reader.read();
  if (command_str.substr(0, 1) != "/") {
    return "process message";
  }
  auto it = commands.find(command_str);
  if (it == commands.end()) {
    return "invalid command";
  }
  Command command = it->second;
  return handle_command(command, &reader);
}

int main(int argc, char **argv) {
  HttpService http;
  http.GET("/",
           [](const HttpContextPtr &ctx) { return ctx->send("hello world!"); });

  WebSocketService ws;

  ws.onopen = [](const WebSocketChannelPtr &channel, const HttpRequestPtr &req) {
    cout << "connected " << channel->id() << endl;
  };

  ws.onmessage = [](const WebSocketChannelPtr &channel, const std::string &message) {
    string res = dispatch_message(message);
    channel->send(res);
  };

  ws.onclose = [](const WebSocketChannelPtr &channel) {
    cout << "disconnected " << channel->id() << endl;
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
