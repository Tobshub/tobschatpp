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
    if (stream.eof()) {
      return "";
    }
    std::getline(stream, token, ' ');
    tokens.push_back(token);
    return token;
  }

  string read_to_end() {
    if (stream.eof()) {
      return "";
    }
    return stream.str().substr(stream.tellg());
  }
};
