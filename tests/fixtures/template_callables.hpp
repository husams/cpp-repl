#pragma once

#include <string>
#include <utility>

namespace template_callables {
std::string absent(int);
template <class T>
class Person {
  T value_;
  int hidden(int) const;

 protected:
  int protected_step(int) const;

 public:
  explicit Person(T value) : value_(value) {}
  std::string name() const& noexcept;
  static int scale(int);
  int overloaded(int) const;
  int overloaded(double) const;
  int call_hidden(int x) const { return hidden(x) + protected_step(x); }
};

template <>
std::string Person<long>::name() const& noexcept;

template <class T>
std::string describe(T);

template <>
std::string describe<long>(long);

template <class T>
int overloaded(T);
template <class T>
int overloaded(T*);

template <int N>
int count(int);

template <class T>
int same_parameters(T);
template <class T>
double same_parameters(int);

template <class T>
struct Specialized;
template <>
struct Specialized<int> {
  int value() const;
};

struct Formatter {
  template <class T>
  std::string format(T) const;
};

template <class T>
struct Outer {
  template <class U>
  struct Inner {
    int value() const;
  };
};
}  // namespace template_callables
