namespace generation_native {
int missing(int value);
extern int value;
} // namespace generation_native

extern "C" int generated_native_answer() {
  return generation_native::missing(generation_native::value);
}
