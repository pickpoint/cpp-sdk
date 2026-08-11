#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace pickpoint {

class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class InvalidConfigError : public Error {
 public:
  using Error::Error;
};

class AuthError : public Error {
 public:
  using Error::Error;
};

class NotFoundError : public Error {
 public:
  using Error::Error;
};

class ConflictError : public Error {
 public:
  using Error::Error;
};

class ApiError : public Error {
 public:
  ApiError(int status, std::string code, std::string message, std::vector<std::uint8_t> body = {})
      : Error(message.empty() ? code : message),
        status(status),
        code(std::move(code)),
        message_(std::move(message)),
        body(std::move(body)) {}

  int status = 0;
  std::string code;
  std::string message_;
  std::vector<std::uint8_t> body;

  bool is_auth() const {
    return code == "API_AUTH" || code == "REFRESH_FAILED" || status == 401 || status == 402 ||
           status == 403;
  }
  bool is_not_found() const { return code == "NOT_FOUND" || status == 404; }
  bool is_conflict() const { return code == "CONFLICT" || status == 409; }
};

}  // namespace pickpoint
