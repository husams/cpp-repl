extern "C" int ctk_nested_middle_value();

extern "C" int ctk_nested_root_value() {
  return ctk_nested_middle_value() + 7;
}
