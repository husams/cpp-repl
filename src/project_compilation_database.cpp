#include "project_compilation_database.hpp"

#include <filesystem>

#include "clang/Driver/Types.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "clang/Tooling/JSONCompilationDatabase.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/VirtualFileSystem.h"
#if LLVM_VERSION_MAJOR >= 22
#include "clang/Options/Options.h"
#else
#include "clang/Driver/Options.h"
#endif

namespace example {
namespace fs = std::filesystem;
namespace {

fs::path normalized(const fs::path& path) {
  std::error_code error;
  const auto result = fs::weakly_canonical(path, error);
  return error ? path.lexically_normal() : result;
}

bool cpp_command(const clang::tooling::CompileCommand& command) {
  auto extension = fs::path(command.Filename).extension().string();
  if (!extension.empty()) extension.erase(0, 1);
  const auto type = clang::driver::types::lookupTypeForExtension(extension);
  if (clang::driver::types::isCXX(type)) return true;
  for (std::size_t i = 0; i < command.CommandLine.size(); ++i) {
    if (command.CommandLine[i] == "-xc++" ||
        command.CommandLine[i] == "-xc++-header" ||
        (command.CommandLine[i] == "-x" && i + 1 < command.CommandLine.size() &&
         command.CommandLine[i + 1].rfind("c++", 0) == 0))
      return true;
  }
  return false;
}

}  // namespace

struct ProjectCompilationDatabase::Impl {
  std::string path;
  std::vector<clang::tooling::CompileCommand> commands;
};

ProjectCompilationDatabase::ProjectCompilationDatabase()
    : impl_(std::make_unique<Impl>()) {}
ProjectCompilationDatabase::~ProjectCompilationDatabase() = default;

std::unique_ptr<ProjectCompilationDatabase> ProjectCompilationDatabase::load(
    std::string& error) {
  error.clear();
  auto result = std::make_unique<ProjectCompilationDatabase>();
  std::error_code ec;
  auto directory = fs::current_path(ec);
  if (ec) {
    error = "cannot find startup directory: " + ec.message();
    return nullptr;
  }
  for (;;) {
    const std::vector<fs::path> candidates = {
        directory / "compile_commands.json",
        directory / "build/compile_commands.json",
        directory / "build-linux/compile_commands.json"};
    for (const auto& candidate : candidates) {
      if (!fs::exists(candidate, ec)) continue;
      auto database = clang::tooling::JSONCompilationDatabase::loadFromFile(
          candidate.string(), error,
          clang::tooling::JSONCommandLineSyntax::AutoDetect);
      if (!database) {
        error = "cannot load " + candidate.string() + ": " + error;
        return nullptr;
      }
      result->impl_->path = normalized(candidate).string();
      auto expanded = clang::tooling::expandResponseFiles(
          std::move(database), llvm::vfs::getRealFileSystem());
      result->impl_->commands = expanded->getAllCompileCommands();
      for (auto& command : result->impl_->commands) {
        auto working_directory = fs::path(command.Directory);
        if (working_directory.is_relative())
          working_directory = candidate.parent_path() / working_directory;
        command.Directory = normalized(working_directory).string();
        auto source = fs::path(command.Filename);
        if (source.is_relative()) source = fs::path(command.Directory) / source;
        command.Filename = normalized(source).string();
      }
      return result;
    }
    const auto parent = directory.parent_path();
    if (parent == directory || parent.empty()) break;
    directory = parent;
  }
  return result;
}

std::optional<SourceCompileOptions> ProjectCompilationDatabase::options_for(
    const std::string& file) const {
  const auto target = normalized(file);
  const clang::tooling::CompileCommand* chosen = nullptr;
  for (const auto& command : impl_->commands) {
    if (normalized(command.Filename) == target) {
      chosen = &command;
      break;
    }
    if (fs::path(command.Filename).parent_path() != target.parent_path() ||
        !cpp_command(command))
      continue;
    const bool same_stem = fs::path(command.Filename).stem() == target.stem();
    const bool chosen_stem =
        chosen && fs::path(chosen->Filename).stem() == target.stem();
    if (!chosen || (same_stem && !chosen_stem) ||
        (same_stem == chosen_stem && command.Filename < chosen->Filename))
      chosen = &command;
  }
  if (!chosen) return std::nullopt;
  auto command =
      clang::tooling::transferCompileCommand(*chosen, target.string());
  auto arguments =
      clang::tooling::getClangStripOutputAdjuster()(command.CommandLine, file);
  arguments =
      clang::tooling::getClangStripDependencyFileAdjuster()(arguments, file);
  std::vector<const char*> argv;
  for (std::size_t i = 1; i < arguments.size(); ++i)
    argv.push_back(arguments[i].c_str());
  unsigned missing_index = 0, missing_count = 0;
#if LLVM_VERSION_MAJOR >= 22
  namespace options = clang::options;
  const auto& table = clang::getDriverOptTable();
#else
  namespace options = clang::driver::options;
  const auto& table = clang::driver::getDriverOptTable();
#endif
  auto parsed = table.ParseArgs(argv, missing_index, missing_count,
                                llvm::opt::Visibility(options::ClangOption));
  llvm::opt::ArgStringList flags;
  for (const auto* argument : parsed) {
    if (argument->getOption().matches(options::OPT_INPUT) ||
        argument->getOption().matches(options::OPT__DASH_DASH) ||
        argument->getOption().matches(options::OPT_c) ||
        argument->getOption().matches(options::OPT_S) ||
        argument->getOption().matches(options::OPT_E) ||
        argument->getOption().matches(options::OPT_fsyntax_only))
      continue;
    argument->render(parsed, flags);
  }
  SourceCompileOptions result;
  result.directory = command.Directory;
  result.source = chosen->Filename;
  for (const auto* flag : flags)
    if (std::string(flag) != "--") result.arguments.emplace_back(flag);
  // A header is parsed into the persistent C++ session, rather than emitted as
  // a PCH.
  result.arguments.insert(
      result.arguments.end(),
      {"-x", "c++", "-working-directory", result.directory});
  return result;
}

const std::string& ProjectCompilationDatabase::path() const {
  return impl_->path;
}

std::optional<std::vector<std::string>> normalize_compile_flags(
    const std::vector<std::string>& flags, const std::string& directory,
    std::string& error) {
  error.clear();
  std::vector<const char*> argv;
  for (const auto& flag : flags) argv.push_back(flag.c_str());
  unsigned missing_index = 0, missing_count = 0;
#if LLVM_VERSION_MAJOR >= 22
  namespace options = clang::options;
  const auto& table = clang::getDriverOptTable();
#else
  namespace options = clang::driver::options;
  const auto& table = clang::driver::getDriverOptTable();
#endif
  auto parsed = table.ParseArgs(argv, missing_index, missing_count,
                                llvm::opt::Visibility(options::ClangOption));
  if (missing_count) {
    error = "missing value for compiler option: " + flags[missing_index];
    return std::nullopt;
  }
  for (auto* argument : parsed) {
    if (argument->getOption().matches(options::OPT_UNKNOWN)) {
      error = "unknown compiler option: " + argument->getSpelling().str();
      return std::nullopt;
    }
    if (argument->getOption().matches(options::OPT_INPUT)) {
      error = "expected compiler flags, found: " +
              std::string(argument->getValue());
      return std::nullopt;
    }
    const auto name = argument->getOption().getName();
    const bool header =
        name == "include" || name == "imacros" || name == "include-pch";
    const bool path = header || name == "I" || name == "F" || name == "B" ||
                      name == "isystem" || name == "iquote" ||
                      name == "idirafter" || name == "iframework" ||
                      name == "iframeworkwithsysroot" || name == "isysroot" ||
                      name == "-sysroot=" || name == "resource-dir" ||
                      name == "resource-dir=" || name == "working-directory";
    if (!path || argument->getNumValues() != 1) continue;
    const auto value = fs::path(argument->getValue());
    if (value.is_absolute()) continue;
    const auto absolute = (fs::path(directory) / value).lexically_normal();
    std::error_code ec;
    if (!header || fs::is_regular_file(absolute, ec))
      argument->getValues()[0] = parsed.MakeArgString(absolute.string());
  }
  llvm::opt::ArgStringList rendered;
  for (auto* argument : parsed) argument->render(parsed, rendered);
  std::vector<std::string> result;
  for (auto* flag : rendered) result.emplace_back(flag);
  return result;
}

}  // namespace example
