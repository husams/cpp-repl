extern "C" int ctk_nested_leaf_value();

extern "C" int ctk_nested_middle_value() {
  return ctk_nested_leaf_value() + 13;
}
