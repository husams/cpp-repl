#include "unresolved_storage.hpp"

namespace unresolved_storage_fixture {

long long State::read() { return value_; }
const char* State::label() { return label_.c_str(); }
const void* State::value_address() { return &value_; }

}  // namespace unresolved_storage_fixture
