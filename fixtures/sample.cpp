#include <iostream>

int add(int a, int b) {
return a + b;
}

class Point  {
  int x = 0;
  int y = 2;
public:
  Point(int a, int b): x(a), y(x)
  {}
  void inc(int s) {
    x = add(x, s);
    y = add(x, s);
  }
};
