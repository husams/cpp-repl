#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include "library_manager.hpp"

#ifndef DIRECT_ROOT_LIBRARY
#error "DIRECT_ROOT_LIBRARY must name the direct dependency fixture"
#endif
#ifndef NESTED_ROOT_LIBRARY
#error "NESTED_ROOT_LIBRARY must name the nested dependency fixture"
#endif
#ifndef RETRY_ROOT_LIBRARY
#error "RETRY_ROOT_LIBRARY must name the retry fixture root"
#endif
#ifndef RETRY_DEPENDENCY_DIRECTORY
#error "RETRY_DEPENDENCY_DIRECTORY must name the retry dependency directory"
#endif

namespace example {
namespace {

template <typename Function>
Function symbol(void* handle, const char* name) {
  return reinterpret_cast<Function>(dlsym(handle, name));
}

class TemporaryCurrentDirectory {
public:
  explicit TemporaryCurrentDirectory(const std::string& label)
      : previous_(std::filesystem::current_path())
      , directory_(std::filesystem::temp_directory_path() /
                   ("ctk-library-dependency-" + label + "-" + std::to_string(::getpid()))) {
    std::filesystem::create_directories(directory_);
    std::filesystem::current_path(directory_);
  }

  ~TemporaryCurrentDirectory() {
    std::error_code ec;
    std::filesystem::current_path(previous_, ec);
    std::filesystem::remove_all(directory_, ec);
  }

private:
  std::filesystem::path previous_;
  std::filesystem::path directory_;
};

std::vector<char> read_binary(const std::filesystem::path& file) {
  std::ifstream input(file, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void* loaded_root_handle(const LibraryManager& libraries, const std::filesystem::path& root) {
  for (const LibraryManager::Loaded& loaded : libraries.loaded()) {
    if (std::filesystem::path(loaded.path).filename() == root.filename()) {
      return loaded.handle;
    }
  }
  return nullptr;
}

TEST(LibraryManagerDependencies, RejectedLoadReleasesStagedSiblingDependencies) {
  const std::filesystem::path library(DIRECT_ROOT_LIBRARY);
  LibraryManager libraries;
  libraries.set_load_hook([](const std::string&) { return "test hook rejection"; });

  const LibraryManager::Result result = libraries.load(library.string());
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.message, "test hook rejection");
  EXPECT_TRUE(libraries.loaded().empty());
  EXPECT_EQ(dlsym(RTLD_DEFAULT, "ctk_direct_root_value"), nullptr);
  EXPECT_EQ(dlsym(RTLD_DEFAULT, "ctk_direct_dependency_value"), nullptr);
}

TEST(LibraryManagerDependencies, DirectPathFindsSiblingAndExportsAreCallable) {
  const std::filesystem::path library(DIRECT_ROOT_LIBRARY);
  TemporaryCurrentDirectory cwd("direct-cwd");
  const std::vector<char> original_bytes = read_binary(library);
  ASSERT_FALSE(original_bytes.empty());

  LibraryManager libraries;
  const LibraryManager::Result result = libraries.load(library.string());
  ASSERT_TRUE(result.ok) << result.message;
  EXPECT_EQ(read_binary(library), original_bytes);
  void* root_handle = loaded_root_handle(libraries, library);
  ASSERT_NE(root_handle, nullptr);
  const auto loaded_value = symbol<int (*)()>(root_handle, "ctk_direct_root_value");
  ASSERT_NE(loaded_value, nullptr);
  EXPECT_EQ(loaded_value(), 42);
}

TEST(LibraryManagerDependencies, AddedPathFindsNestedSiblingDependenciesFromOtherCwd) {
  const std::filesystem::path library(NESTED_ROOT_LIBRARY);
  TemporaryCurrentDirectory cwd("nested-cwd");
  const std::vector<char> original_bytes = read_binary(library);
  ASSERT_FALSE(original_bytes.empty());

  LibraryManager libraries;
  ASSERT_TRUE(libraries.add_path(library.parent_path().string()).ok);
  const LibraryManager::Result result = libraries.load(library.filename().string());
  ASSERT_TRUE(result.ok) << result.message;
  EXPECT_EQ(read_binary(library), original_bytes);
  void* root_handle = loaded_root_handle(libraries, library);
  ASSERT_NE(root_handle, nullptr);
  const auto loaded_value = symbol<int (*)()>(root_handle, "ctk_nested_root_value");
  ASSERT_NE(loaded_value, nullptr);
  EXPECT_EQ(loaded_value(), 37);
}

TEST(LibraryManagerDependencies, FailedLoadCanRetryAfterDependencyDirectoryIsAdded) {
  const std::filesystem::path root(RETRY_ROOT_LIBRARY);
  const std::vector<char> original_bytes = read_binary(root);
  ASSERT_FALSE(original_bytes.empty());
  LibraryManager libraries;

  const LibraryManager::Result first = libraries.load(root.string());
  EXPECT_FALSE(first.ok);
  EXPECT_FALSE(first.message.empty());
  EXPECT_EQ(read_binary(root), original_bytes);
  EXPECT_TRUE(libraries.loaded().empty());

  ASSERT_TRUE(libraries.add_path(RETRY_DEPENDENCY_DIRECTORY).ok);
  const LibraryManager::Result retry = libraries.load(root.string());
  ASSERT_TRUE(retry.ok) << retry.message;
  EXPECT_EQ(read_binary(root), original_bytes);
  void* root_handle = loaded_root_handle(libraries, root);
  ASSERT_NE(root_handle, nullptr);
  const auto loaded_value = symbol<int (*)()>(root_handle, "ctk_retry_root_value");
  ASSERT_NE(loaded_value, nullptr);
  EXPECT_EQ(loaded_value(), 42);
}

} // namespace
} // namespace example
