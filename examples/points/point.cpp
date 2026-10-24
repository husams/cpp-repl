#include "point.hpp"

int add(int a, int b) {
  return a + b;
}

int Point::inc(int n) {
  return add(n, 1);
}

int Point::run(int n) {
  return inc(n);
}

int Point::double_it(int n) {
  return add(n, n);
}
