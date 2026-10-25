extern "C" int ctk_retry_dependency_value();

extern "C" int ctk_retry_root_value() {
  return ctk_retry_dependency_value() + 19;
}
