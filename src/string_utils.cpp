#include <cctype> // For std::isspace
#include <string>
#include "string_utils.hpp"

namespace string_utils {
// Function to trim leading whitespace
std::string ltrim(const std::string &s) {
  size_t start = 0;
  while (start < s.length() && std::isspace(s[start])) {
    start++;
  }
  return s.substr(start);
}

// Function to trim trailing whitespace
std::string rtrim(const std::string &s) {
  size_t end = s.length();
  while (end > 0 && std::isspace(s[end - 1])) {
    end--;
  }
  return s.substr(0, end);
}

// Function to trim both leading and trailing whitespace
std::string trim(const std::string &s) { return ltrim(rtrim(s)); }

} // namespace string_utils
