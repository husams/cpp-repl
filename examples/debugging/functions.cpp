int debug_increment(int value) {
  int result = value + 1;
  return result;
}

int debug_twice(int value) {
  int doubled = value * 2;
  int result = debug_increment(doubled);
  return result;
}
