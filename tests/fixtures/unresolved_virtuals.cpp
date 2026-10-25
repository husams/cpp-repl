#include "unresolved_virtuals.hpp"

#include <typeinfo>

namespace virtual_fixture {
void NativeTable::anchor() {}

int through_service(const Service& service, int x) {
  return service.compute(x);
}

int through_api(const API& api, int x) {
  return api.compute(x);
}

bool identifies_implementation(API* api) {
  return dynamic_cast<Implementation*>(api) && typeid(*api) == typeid(Implementation);
}

extern "C" int virtual_non_pure_probe(int x) {
  Service service(40);
  return through_service(service, x);
}

extern "C" int virtual_override_probe(int x) {
  Implementation implementation(100);
  return through_api(implementation, x);
}

extern "C" int virtual_rtti_probe() {
  Implementation implementation(100);
  return identifies_implementation(&implementation);
}

extern "C" int virtual_native_table_probe(int x) {
  NativeTable implementation(200);
  return through_api(implementation, x);
}

extern "C" int virtual_base_probe(int x) {
  VirtualBase implementation(300);
  return through_api(implementation, x);
}

CovariantAPI* through_covariant(CovariantAPI& object) {
  return object.self();
}

extern "C" int virtual_covariant_probe() {
  Covariant object;
  return through_covariant(object) == static_cast<CovariantAPI*>(&object);
}
} // namespace virtual_fixture
