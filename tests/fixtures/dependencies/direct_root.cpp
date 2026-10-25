extern "C" int ctk_direct_dependency_value();

extern "C" int ctk_direct_root_value() {
  return ctk_direct_dependency_value() + 11;
}
