#pragma once

namespace virtual_fixture {
class Service {
  int seed_;

public:
  explicit Service(int seed)
      : seed_(seed) {}

  virtual ~Service() = default;
  virtual int compute(int) const;
};

struct API {
  virtual ~API() = default;
  virtual int compute(int) const = 0;
};

struct Padding {
  virtual ~Padding() = default;

  virtual int tag() const { return 13; }

  int padding = 7;
};

class Implementation : public Padding, public API {
  int bias_;

public:
  explicit Implementation(int bias)
      : bias_(bias) {}

  int compute(int) const override;
};

class NativeTable : public Padding, public API {
  int bias_;

public:
  explicit NativeTable(int bias)
      : bias_(bias) {}

  virtual void anchor();
  int compute(int) const override;
};

class VirtualBase : public virtual API {
  int bias_;

public:
  explicit VirtualBase(int bias)
      : bias_(bias) {}

  int compute(int) const override;
};

struct CovariantAPI {
  virtual ~CovariantAPI() = default;
  virtual CovariantAPI* self() = 0;
};

class Covariant : public Padding, public CovariantAPI {
public:
  Covariant* self() override;
};

extern "C" int virtual_non_pure_probe(int);
extern "C" int virtual_override_probe(int);
extern "C" int virtual_rtti_probe();
extern "C" int virtual_native_table_probe(int);
extern "C" int virtual_base_probe(int);
extern "C" int virtual_covariant_probe();
} // namespace virtual_fixture
