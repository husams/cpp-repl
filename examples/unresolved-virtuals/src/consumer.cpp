#include <typeinfo>

#include "api.hpp"

namespace virtual_demo {
int call_service(const Service& service, int x) {
  return service.compute(x);
}

int call_api(const API& api, int x) {
  return api.compute(x);
}

bool identifies_implementation(API* api) {
  return dynamic_cast<Implementation*>(api) && typeid(*api) == typeid(Implementation);
}

extern "C" int non_pure_probe(int x) {
  Service service(40);
  return call_service(service, x);
}

extern "C" int override_probe(int x) {
  Implementation implementation(100);
  return call_api(implementation, x);
}

extern "C" int rtti_probe() {
  Implementation implementation(100);
  return identifies_implementation(&implementation);
}

extern "C" int direct_probe() {
  Service service(40);
  Implementation implementation(100);
  return service.Service::compute(2) == 42 && implementation.Implementation::compute(3) == 106;
}
} // namespace virtual_demo
