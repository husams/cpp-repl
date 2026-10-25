#pragma once

namespace demo {
struct Value {};

class Base {
  int secret_;
  void hidden();

protected:
  double weight_;
  void adjust();

public:
  Value value;
  static int count;
  virtual int run(int) const;
  static bool ready();
};

struct Left : virtual Base {
  int left;
  void left_method();
};

struct Right : virtual Base {
  int right;
};

class Record : private Left, public Right {
  int own_;
  void own_method();

public:
  int run(int) const override;
  double run(double);
  template <class T>
  T convert(T);
};

union Choice {
  int number;
  double real;
  void clear();

private:
  char tag_;
};
} // namespace demo
