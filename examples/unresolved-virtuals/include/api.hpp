#pragma once

namespace virtual_demo {
// Case 1: a non-pure virtual method whose definition is missing.
class Service {
  int seed_;

public:
  explicit Service(int seed)
      : seed_(seed) {}

  virtual ~Service() = default;
  virtual int compute(int) const;
};

// Case 2: a concrete override of a pure virtual base method.
struct API {
  virtual ~API() = default;
  virtual int compute(int) const = 0;
};

struct Padding {
  virtual ~Padding() = default;
  int padding = 7;
};

class Implementation : public Padding, public API {
  int bias_;

public:
  explicit Implementation(int bias)
      : bias_(bias) {}

  int compute(int) const override;
};

// These wrappers are compiled into the supplied library, before the REPL runs.
int call_service(const Service&, int);
int call_api(const API&, int);
extern "C" int non_pure_probe(int);
extern "C" int override_probe(int);
extern "C" int rtti_probe();
extern "C" int direct_probe();
} // namespace virtual_demo
