#include <dlfcn.h>
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  if (void* library = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL)) {
    dlclose(library);
    std::cerr << "UNEXPECTED_LOAD_SUCCESS\n";
    return 1;
  }
  const char* error = dlerror();
  std::cout << "EXPECTED_LOAD_FAILURE: " << (error ? error : "unknown") << '\n';
}
