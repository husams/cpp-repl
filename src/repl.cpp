#include "repl.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "commands.hpp"
#include "cpp_lexer.hpp"
#include "editor_call.hpp"
#include "editor_input.hpp"
#include "editor_ui.hpp"
#include "highlight_repl.hpp"
#include "runtime_inspection.hpp"
#include "session_commands.hpp"

namespace example {

namespace {

using replxx::Replxx;

bool starts_with(const std::string& s, std::string_view prefix) {
  return s.compare(0, prefix.size(), prefix) == 0;
}

bool is_identifier_byte(unsigned char c) {
  return std::isalnum(c) != 0 || c == '_' || c >= 0x80;
}

Replxx::completions_t cpp_completions(CppInterpreter& cpp, Replxx& rx, const std::string& input,
                                      int& context_len,
                                      std::unordered_map<std::string, std::string>& identities) {
  Replxx::completions_t out;
  const auto result = cpp.complete(input);
  context_len = static_cast<int>(utf8_length(result.prefix));

  const auto state = rx.get_state();
  const std::string buffer = state.text();
  const std::size_t cursor = utf8_byte_offset(buffer, state.cursor_position());
  std::size_t identifier_end = cursor;
  while (identifier_end < buffer.size() &&
         is_identifier_byte(static_cast<unsigned char>(buffer[identifier_end]))) {
    ++identifier_end;
  }
  const int erase_identifier = static_cast<int>(
      utf8_length(std::string_view(buffer).substr(cursor, identifier_end - cursor)));
  const auto suffix_after_identifier = std::string_view(buffer).substr(identifier_end);
  const auto first_nonblank = suffix_after_identifier.find_first_not_of(" \t\r\n");
  const bool existing_call =
      first_nonblank != std::string_view::npos && suffix_after_identifier[first_nonblank] == '(';

  for (const auto& item : result.items) {
    auto display = item.label;
    if (!item.detail.empty()) {
      display += " -> " + item.detail;
    }
    display += " [" + std::string(semantic_kind_name(item.kind)) + "]";
    auto insertion = item.text;
    int cursor_offset = 0;
    int erase_after = erase_identifier;
    if (item.is_header_path) {
      const auto end = buffer.find_first_of("\"<>\r\n", cursor);
      const auto count = (end == std::string::npos ? buffer.size() : end) - cursor;
      erase_after = static_cast<int>(utf8_length(std::string_view(buffer).substr(cursor, count)));
    }
    if (item.insert_call &&
        (item.kind == SemanticKind::Function || item.kind == SemanticKind::Method)) {
      if (existing_call) {
        cursor_offset =
            static_cast<int>(utf8_length(suffix_after_identifier.substr(0, first_nonblank + 1)));
      } else {
        insertion += "()";
        cursor_offset = -1;
      }
    }
    identities[display] = item.identity;
    out.emplace_back(insertion, display, semantic_color(item.kind), cursor_offset, erase_after);
  }
  return out;
}

// Start of the path being typed at the end of a %lib / %add-path / %add line:
// after the last blank, or for %add-path also after the last ':'.
std::size_t current_path_begin(const std::string& input, const ParsedCommand& cmd) {
  const char* stops = cmd.kind == CommandKind::AddPath ? ": \t" : " \t";
  const std::size_t pos = input.find_last_of(stops);
  return pos == std::string::npos || pos < cmd.arg_begin ? cmd.arg_begin : pos + 1;
}

// Tab: C++ is completed by the interpreter; `%` + Tab completes command
// names, Tab after "%lib " / "%add-path " / "%add " the filesystem path being
// typed and after "%class " the session's class names.
Replxx::completions_t complete(CppInterpreter& cpp, const std::string& input, int& context_len,
                               Replxx& rx,
                               std::unordered_map<std::string, std::string>& identities) {
  Replxx::completions_t out;
  if (!is_command_line(input)) {
    return cpp_completions(cpp, rx, input, context_len, identities);
  }
  const ParsedCommand cmd = parse_command(input);

  if (cmd.kind == CommandKind::OptionValue) {
    const auto typed = input.substr(cmd.name_begin);
    context_len = static_cast<int>(utf8_length(typed));
    for (const auto name : repl_option_names) {
      const auto candidate = "$" + std::string(name);
      if (starts_with(candidate, typed)) {
        out.emplace_back(candidate);
      }
    }
    return out;
  }
  if (cmd.kind == CommandKind::Set && cmd.name_end < input.size()) {
    const auto equal = input.find('=', cmd.arg_begin);
    if (equal == std::string::npos) {
      const auto typed = input.substr(cmd.arg_begin);
      context_len = static_cast<int>(utf8_length(typed));
      for (const auto name : repl_option_names) {
        if (starts_with(std::string(name), typed)) {
          out.emplace_back(std::string(name));
        }
      }
    } else {
      const auto begin = input.find_first_not_of(" \t", equal + 1);
      const auto typed = begin == std::string::npos ? std::string() : input.substr(begin);
      context_len = static_cast<int>(utf8_length(typed));
      for (const auto* value : {"true", "false"}) {
        if (starts_with(value, typed)) {
          out.emplace_back(value);
        }
      }
    }
    return out;
  }

  if (cmd.code && cmd.incomplete) {
    return cpp_completions(cpp, rx, cmd.code->value, context_len, identities);
  }

  if (cmd.name_end == input.size()) { // still typing the command word
    const std::string token = input.substr(cmd.name_begin);
    context_len = static_cast<int>(utf8_length(token));
    for (const std::string& name : command_names()) {
      if (starts_with(name, token)) {
        out.emplace_back(name);
      }
    }
    return out;
  }

  if (cmd.kind == CommandKind::Lib || cmd.kind == CommandKind::AddPath ||
      cmd.kind == CommandKind::Add || cmd.kind == CommandKind::Reload) {
    const std::string typed = input.substr(current_path_begin(input, cmd));
    context_len = static_cast<int>(utf8_length(typed));
    for (const std::string& p : complete_path(typed, cmd.kind == CommandKind::AddPath)) {
      out.emplace_back(p);
    }
  } else if (cmd.kind == CommandKind::CompileOpts || cmd.kind == CommandKind::AddCompileOpt ||
             cmd.kind == CommandKind::SetCompileOpts || cmd.kind == CommandKind::UseCompileOpts) {
    // Only the first operand can be a filename. Flag values may themselves be
    // paths; do not mistake those for the scope operand.
    auto typed = input.substr(cmd.arg_begin);
    if (!typed.empty() && typed.front() == '-') {
      return out;
    }
    char quote = 0;
    if (!typed.empty() && (typed.front() == '"' || typed.front() == '\'')) {
      quote = typed.front();
      typed.erase(0, 1);
      const auto close = typed.find(quote);
      if (close != std::string::npos) {
        if (close + 1 != typed.size()) {
          return out;
        }
        typed.pop_back();
      }
    } else if (typed.find_first_of(" \t") != std::string::npos) {
      return out;
    }
    context_len = static_cast<int>(utf8_length(typed));
    for (auto path : complete_path(typed, false)) {
      if (quote && !path.empty() && path.back() != '/') {
        path += quote;
      }
      out.emplace_back(path);
    }
  } else if (cmd.kind == CommandKind::Libs && !cmd.all_libraries) {
    const std::string typed = input.substr(cmd.arg_begin);
    context_len = static_cast<int>(utf8_length(typed));
    if (starts_with("all", typed)) {
      out.emplace_back("all");
    }
  } else if (cmd.kind == CommandKind::LibInfo) {
    const std::string typed = input.substr(cmd.arg_begin);
    context_len = static_cast<int>(utf8_length(typed));
    for (const auto& library : loaded_libraries()) {
      if (starts_with(library.name, typed)) {
        out.emplace_back(library.name);
      }
    }
  } else if (cmd.kind == CommandKind::Methods || cmd.kind == CommandKind::Fields) {
    const auto args = input.substr(cmd.arg_begin);
    const auto first_blank = args.find_first_of(" \t");
    if (first_blank == std::string::npos) {
      context_len = static_cast<int>(utf8_length(args));
      for (const auto& name : cpp.class_names()) {
        if (starts_with(name, args)) {
          out.emplace_back(name);
        }
      }
    } else {
      const auto begin = input.find_last_of(" \t") + 1;
      const auto typed = input.substr(begin);
      context_len = static_cast<int>(utf8_length(typed));
      const auto previous_end = input.find_last_not_of(" \t", begin - 1);
      const auto previous_begin = input.find_last_of(" \t", previous_end);
      const auto previous = input.substr(previous_begin + 1, previous_end - previous_begin);
      if (previous == "access") {
        for (const auto* value : {"public", "private", "protected", "all"}) {
          if (starts_with(value, typed)) {
            out.emplace_back(value);
          }
        }
      } else if (previous == "include-parents") {
        for (const auto* value : {"true", "false"}) {
          if (starts_with(value, typed)) {
            out.emplace_back(value);
          }
        }
      } else {
        if (!cmd.access_filter && starts_with("access", typed)) {
          out.emplace_back("access");
        }
        if (!cmd.parents_filter && starts_with("include-parents", typed)) {
          out.emplace_back("include-parents");
        }
      }
    }
  } else if (cmd.kind == CommandKind::Class || cmd.kind == CommandKind::Info ||
             cmd.kind == CommandKind::Type ||
             cmd.kind == CommandKind::DefineVariable) {
    const std::string typed = input.substr(cmd.arg_begin);
    context_len = static_cast<int>(utf8_length(typed));
    for (const std::string& name :
         cmd.kind == CommandKind::Class ? cpp.class_names() : cpp.symbol_names()) {
      if (starts_with(name, typed)) {
        out.emplace_back(name);
      }
    }
  } else if (cmd.kind == CommandKind::Symbols) {
    // A regex can itself start with "lib" or "name"; complete only keywords.
    for (const auto* filter : {&cmd.library_regex, &cmd.name_regex}) {
      if (*filter && (*filter)->span.end == input.size()) {
        return out;
      }
    }
    auto last_filter = cmd.library_regex && cmd.name_regex
                           ? std::max(cmd.library_regex->span.end, cmd.name_regex->span.end)
                       : cmd.library_regex ? cmd.library_regex->span.end
                       : cmd.name_regex    ? cmd.name_regex->span.end
                                           : cmd.name_end;
    if (cmd.all_symbols) {
      last_filter = std::max(last_filter, cmd.all_symbols->end);
    }
    if (cmd.undefined_symbols) {
      last_filter = std::max(last_filter, cmd.undefined_symbols->end);
    }
    const auto remaining = input.find_first_not_of(" \t", last_filter);
    if (remaining != std::string::npos) {
      const auto word_end = input.find_first_of(" \t", remaining);
      // After a complete keyword and a blank the user is entering its regex.
      if (word_end != std::string::npos) {
        return out;
      }
    }
    const auto begin = input.find_last_of(" \t");
    const auto typed = input.substr(begin + 1);
    context_len = static_cast<int>(utf8_length(typed));
    if (!cmd.library_regex && starts_with("lib", typed)) {
      out.emplace_back("lib");
    }
    if (!cmd.name_regex && starts_with("name", typed)) {
      out.emplace_back("name");
    }
    if (!cmd.all_symbols && starts_with("all", typed)) {
      out.emplace_back("all");
    }
    if (!cmd.undefined_symbols && starts_with("undefined", typed)) {
      out.emplace_back("undefined");
    }
  }
  return out;
}

std::string callable_qualifiers(const std::string& label) {
  const auto close = label.rfind(')');
  if (close == std::string::npos) {
    return {};
  }
  const auto suffix = label.substr(close + 1);
  const auto first = suffix.find_first_not_of(" \t");
  return first == std::string::npos ? std::string() : suffix.substr(first);
}

Replxx::hints_t cpp_hints(CppInterpreter& cpp, const std::string& input, int& context_len,
                          Replxx::Color& color, bool inline_display_supported = true) {
  Replxx::hints_t out;
  if (lex_cpp(input).open_brackets.find('(') == std::string::npos) {
    return out;
  }
  const auto result = cpp.signatures(input);
  if (inline_display_supported && !result.signature_items.empty()) {
    return out;
  }
  context_len = 0;
  color = Replxx::Color::GRAY;
  for (const auto& signature : result.signatures) {
    out.push_back("  " + signature + " [argument " + std::to_string(result.active_parameter + 1) +
                  "]");
  }
  return out;
}

Replxx::hints_t hint(CppInterpreter& cpp, const std::string& input, int& context_len,
                     Replxx::Color& color) {
  if (!is_command_line(input)) {
    return cpp_hints(cpp, input, context_len, color, true);
  }
  const ParsedCommand cmd = parse_command(input);
  if (cmd.code && cmd.incomplete) {
    return cpp_hints(cpp, cmd.code->value, context_len, color, false);
  }
  Replxx::hints_t out;
  color = Replxx::Color::GRAY;

  if (cmd.name_end == input.size()) { // matching command names
    const std::string token = input.substr(cmd.name_begin);
    context_len = static_cast<int>(utf8_length(token));
    for (const std::string& name : command_names()) {
      if (starts_with(name, token)) {
        out.emplace_back(name);
      }
    }
  } else if (cmd.arg_begin == input.size()) { // "<cmd> " -> show usage
    const CommandInfo* info = find_command(cmd.name);
    if (info != nullptr && info->usage[0] != '\0') {
      context_len = 0;
      out.emplace_back(info->usage + 1);
    }
  }
  return out;
}

using SelectedSignatures = std::unordered_map<std::size_t, std::string>;

void remap_selected_signatures(SelectedSignatures& selected, const std::string& before,
                               const std::string& after) {
  if (before == after) {
    return;
  }
  std::size_t prefix = 0;
  while (prefix < before.size() && prefix < after.size() && before[prefix] == after[prefix]) {
    ++prefix;
  }
  std::size_t before_suffix = before.size();
  std::size_t after_suffix = after.size();
  while (before_suffix > prefix && after_suffix > prefix &&
         before[before_suffix - 1] == after[after_suffix - 1]) {
    --before_suffix;
    --after_suffix;
  }
  const auto delta =
      static_cast<std::ptrdiff_t>(after_suffix) - static_cast<std::ptrdiff_t>(before_suffix);
  SelectedSignatures remapped;
  for (const auto& [offset, identity] : selected) {
    if (offset < prefix) {
      remapped[offset] = identity;
    } else if (offset == prefix && before_suffix == prefix && after_suffix > prefix) {
      // An overload choice made on a bare function name is anchored at the
      // point where the user will type its opening parenthesis.
      remapped[offset] = identity;
    } else if (offset >= before_suffix) {
      const auto adjusted = static_cast<std::ptrdiff_t>(offset) + delta;
      if (adjusted >= 0) {
        remapped[static_cast<std::size_t>(adjusted)] = identity;
      }
    }
  }
  selected = std::move(remapped);
}

void sync_selected_signatures(const std::shared_ptr<SelectedSignatures>& selected,
                              const std::shared_ptr<std::string>& previous_text,
                              const std::string& current_text) {
  remap_selected_signatures(*selected, *previous_text, current_text);
  *previous_text = current_text;
}

std::string selected_identity_for(const EditorCall& call, const SelectedSignatures& selected) {
  const auto it = selected.find(call.open);
  return it == selected.end() ? std::string() : it->second;
}

CppInterpreter::Completions completions_at_cursor(CppInterpreter& cpp, const std::string& text,
                                                  std::size_t cursor);
CppInterpreter::Completions signatures_at_cursor(CppInterpreter& cpp, const std::string& text,
                                                 std::size_t cursor);

const CompletionItem* selected_signature(const CppInterpreter::Completions& result,
                                         const std::string& identity) {
  if (!identity.empty()) {
    const auto it = std::find_if(result.signature_items.begin(), result.signature_items.end(),
                                 [&](const auto& item) { return item.identity == identity; });
    if (it != result.signature_items.end()) {
      return &*it;
    }
  }
  return result.signature_items.empty() ? nullptr : &result.signature_items.front();
}

const CompletionItem* enter_signature(const CppInterpreter::Completions& result,
                                      const EditorCall& call, const std::string& identity) {
  const auto* selected = selected_signature(result, identity);
  const bool empty_call =
      call.arguments.size() == 1 && call.arguments.front().begin == call.arguments.front().end;
  const bool has_other_empty_slot =
      std::any_of(call.arguments.begin(), call.arguments.end(),
                  [](const auto& argument) { return argument.begin == argument.end; }) &&
      !empty_call;
  if (has_other_empty_slot) {
    return selected;
  }

  const std::size_t supplied_arguments = empty_call ? 0 : call.arguments.size();
  const CompletionItem* compatible = nullptr;
  for (const auto& signature : result.signature_items) {
    if (supplied_arguments > signature.parameters.size() &&
        (signature.parameters.empty() || !signature.parameters.back().is_pack)) {
      continue;
    }
    bool accepts_omitted_arguments = true;
    for (std::size_t i = supplied_arguments; i < signature.parameters.size(); ++i) {
      if (!signature.parameters[i].has_default && !signature.parameters[i].is_pack) {
        accepts_omitted_arguments = false;
        break;
      }
    }
    if (!accepts_omitted_arguments) {
      continue;
    }
    if (!compatible || signature.identity == (selected ? selected->identity : std::string())) {
      compatible = &signature;
    }
  }
  return compatible ? compatible : selected;
}

struct CachedSignature {
  std::string identity;
  std::size_t context = 0;
  std::vector<CompletionItem> signatures;
};

using SignatureCache = std::unordered_map<std::size_t, CachedSignature>;

std::vector<CompletionItem> signatures_for_call(CppInterpreter& cpp, const std::string& text,
                                                const EditorCall& call, std::size_t context,
                                                const SelectedSignatures& selected,
                                                SignatureCache& cache,
                                                bool reuse_cached_context = false) {
  const std::string identity = selected_identity_for(call, selected);
  const auto cached = cache.find(call.open);
  if (cached != cache.end() && cached->second.identity == identity &&
      (cached->second.context == context || reuse_cached_context)) {
    return cached->second.signatures;
  }
  const auto completion = signatures_at_cursor(cpp, text, context);
  CachedSignature entry{identity, context, completion.signature_items};
  cache[call.open] = std::move(entry);
  return cache[call.open].signatures;
}

std::optional<CompletionItem> signature_for_call(CppInterpreter& cpp, const std::string& text,
                                                 const EditorCall& call, std::size_t context,
                                                 const SelectedSignatures& selected,
                                                 SignatureCache& cache) {
  const auto signatures = signatures_for_call(cpp, text, call, context, selected, cache);
  CppInterpreter::Completions completion;
  completion.signature_items = signatures;
  const auto* signature = selected_signature(completion, selected_identity_for(call, selected));
  return signature == nullptr ? std::nullopt : std::optional<CompletionItem>(*signature);
}

std::vector<EditorCall> completed_calls(const std::string& text) {
  std::vector<EditorCall> calls;
  for (const Token& token : lex_cpp(text).tokens) {
    if (text.compare(token.begin, token.end - token.begin, ")") != 0) {
      continue;
    }
    auto call = editor_call_at_cursor(text, token.begin);
    if (call && call->has_close && call->close == token.begin &&
        std::none_of(calls.begin(), calls.end(),
                     [&](const EditorCall& item) { return item.open == call->open; })) {
      calls.push_back(std::move(*call));
    }
  }
  return calls;
}

struct OverloadMenu {
  std::string source;
  std::size_t call_open = 0;
  std::size_t cursor = 0;
  std::size_t prefix_begin = 0;
  bool from_prefix = false;
  std::vector<CompletionItem> items;
  std::size_t selected = 0;
};

struct StaleOverloadMenu {
  bool pending = false;
  std::string source;
};

using ActiveOverloadMenu = std::optional<OverloadMenu>;

std::string overload_menu_label(const CompletionItem& item) {
  std::string label = item.label;
  if (!item.detail.empty()) {
    label += " -> " + item.detail;
  }
  return label;
}

bool open_overload_menu(CppInterpreter& cpp, const std::string& text, std::size_t cursor,
                        const SelectedSignatures& selected, ActiveOverloadMenu& menu) {
  // Command lines have their own completion parser, including CPP payloads
  // embedded in commands. Let that route handle them before querying C++ on
  // the raw command text for overload-menu candidates.
  if (is_command_line(text)) {
    return false;
  }
  std::size_t context = cursor;
  std::size_t anchor = 0;
  std::size_t prefix_begin = cursor;
  std::string typed_prefix;
  auto call = editor_call_at_cursor(text, cursor);
  if (!call) {
    std::size_t after_name = cursor;
    while (after_name < text.size() && (text[after_name] == ' ' || text[after_name] == '\t' ||
                                        text[after_name] == '\r' || text[after_name] == '\n')) {
      ++after_name;
    }
    if (after_name < text.size() && text[after_name] == '(') {
      call = editor_call_at_cursor(text, after_name + 1);
    }
  }
  const bool in_call = static_cast<bool>(call);
  if (call) {
    anchor = call->open;
    if (!call->arguments.empty() &&
        (call->arguments.size() > 1 ||
         call->arguments.front().begin != call->arguments.front().end)) {
      return false;
    }
    context = std::min(cursor, call->close);
  } else {
    std::size_t begin = cursor;
    while (begin > 0 && is_identifier_byte(static_cast<unsigned char>(text[begin - 1]))) {
      --begin;
    }
    if (begin == cursor) {
      return false;
    }
    prefix_begin = begin;
    typed_prefix = text.substr(begin, cursor - begin);
    if (cursor < text.size() && is_identifier_byte(static_cast<unsigned char>(text[cursor]))) {
      return false;
    }
    auto before = std::string_view(text).substr(0, begin);
    const auto nonblank = before.find_last_not_of(" \t\r\n");
    if (nonblank != std::string_view::npos) {
      const auto word_begin = before.find_last_of(" \t\r\n", nonblank);
      const auto previous_word =
          before.substr(word_begin == std::string_view::npos ? 0 : word_begin + 1,
                        nonblank - (word_begin == std::string_view::npos ? 0 : word_begin + 1) + 1);
      if (before[nonblank] == '&' || previous_word == "using" || before[nonblank] == '<') {
        return false;
      }
    }
    anchor = cursor;
  }

  const auto result = cpp.complete(text.substr(0, context));
  const auto& source_items =
      result.signature_items.size() >= 2 ? result.signature_items : result.items;
  std::vector<CompletionItem> items;
  std::unordered_set<std::string> callable_names;
  for (const auto& item : source_items) {
    if ((item.kind != SemanticKind::Function && item.kind != SemanticKind::Method) ||
        (!in_call && !item.insert_call)) {
      continue;
    }
    callable_names.insert(item.text);
  }
  std::string callable_name;
  if (in_call && call) {
    // Clang's signature list is already scoped to the callable being edited.
    // Trust it when it resolves to one family; lexical callee extraction below
    // handles ordinary calls and remains the safer fallback for mixed results.
    if (result.signature_items.size() >= 2 && callable_names.size() == 1) {
      callable_name = *callable_names.begin();
    } else {
      std::size_t end = call->open;
      while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
      }
      std::size_t begin = end;
      while (begin > 0 && is_identifier_byte(static_cast<unsigned char>(text[begin - 1]))) {
        --begin;
      }
      if (begin == end) {
        return false;
      }
      const std::string callee = text.substr(begin, end - begin);
      for (const auto& item : source_items) {
        if ((item.kind == SemanticKind::Function || item.kind == SemanticKind::Method) &&
            (in_call || item.insert_call) &&
            (item.text == callee ||
             (item.text.size() > callee.size() + 2 &&
              item.text.compare(item.text.size() - callee.size(), callee.size(), callee) == 0 &&
              item.text.compare(item.text.size() - callee.size() - 2, 2, "::") == 0))) {
          callable_name = item.text;
          break;
        }
      }
    }
  } else if (callable_names.size() == 1) {
    callable_name = *callable_names.begin();
  } else if (callable_names.size() > 1 && callable_names.count(typed_prefix)) {
    callable_name = typed_prefix;
  }
  if (callable_name.empty()) {
    return false;
  }
  for (const auto& item : source_items) {
    if (item.text == callable_name &&
        (item.kind == SemanticKind::Function || item.kind == SemanticKind::Method) &&
        (in_call || item.insert_call)) {
      items.push_back(item);
    }
  }
  if (items.size() < 2) {
    return false;
  }
  if (call) {
    const std::string identity = selected_identity_for(*call, selected);
    if (std::any_of(items.begin(), items.end(),
                    [&](const auto& item) { return item.identity == identity; })) {
      return false;
    }
  }
  OverloadMenu opened;
  opened.source = text;
  opened.call_open = anchor;
  opened.cursor = cursor;
  opened.prefix_begin = prefix_begin;
  opened.from_prefix = !in_call;
  opened.items = std::move(items);
  menu = std::move(opened);
  return true;
}

bool is_trailing_call(const std::string& text, const EditorCall& call) {
  bool semicolon = false;
  for (const Token& token : lex_cpp(text).tokens) {
    if (token.begin <= call.close) {
      continue;
    }
    if (token.kind == TokenKind::Comment) {
      continue;
    }
    if (!semicolon && text.compare(token.begin, token.end - token.begin, ";") == 0) {
      semicolon = true;
      continue;
    }
    return false;
  }
  return true;
}

CppInterpreter::Completions completions_at_cursor(CppInterpreter& cpp, const std::string& text,
                                                  std::size_t cursor) {
  return cpp.complete(text.substr(0, cursor));
}

CppInterpreter::Completions signatures_at_cursor(CppInterpreter& cpp, const std::string& text,
                                                 std::size_t cursor) {
  return cpp.signatures(text.substr(0, cursor));
}

bool should_complete_argument_prefix(CppInterpreter& cpp, const std::string& text,
                                     const EditorCall& call, std::size_t cursor) {
  if (call.active_argument >= call.arguments.size()) {
    return false;
  }
  const auto& argument = call.arguments[call.active_argument];
  if (cursor < argument.begin || cursor > argument.end) {
    return false;
  }
  std::size_t token_begin = cursor;
  while (token_begin > argument.begin &&
         is_identifier_byte(static_cast<unsigned char>(text[token_begin - 1]))) {
    --token_begin;
  }
  if (token_begin == cursor) {
    return false;
  }
  const std::string typed = text.substr(token_begin, cursor - token_begin);
  const auto candidates = completions_at_cursor(cpp, text, cursor);
  const bool exact_noncallable =
      std::any_of(candidates.items.begin(), candidates.items.end(), [&](const auto& item) {
        return item.text == typed && !(item.insert_call && (item.kind == SemanticKind::Function ||
                                                            item.kind == SemanticKind::Method));
      });
  if (exact_noncallable) {
    return false;
  }
  return std::any_of(candidates.items.begin(), candidates.items.end(), [&](const auto& item) {
    if (!starts_with(item.text, typed)) {
      return false;
    }
    return item.text.size() > typed.size() ||
           (item.insert_call &&
            (item.kind == SemanticKind::Function || item.kind == SemanticKind::Method));
  });
}

std::string parameter_label(const CompletionParameter& parameter, std::size_t index) {
  return parameter.name.empty() ? "arg" + std::to_string(index + 1) : parameter.name;
}

std::vector<std::size_t> completed_call_open_offsets(const std::string& text) {
  std::vector<std::size_t> offsets;
  for (const Token& token : lex_cpp(text).tokens) {
    if (text.compare(token.begin, token.end - token.begin, "(") != 0) {
      continue;
    }
    auto call = editor_call_at_cursor(text, token.end);
    if (!call || call->open != token.begin || !call->has_close) {
      continue;
    }
    offsets.push_back(token.begin);
  }
  std::sort(offsets.begin(), offsets.end(), std::greater<>());
  offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
  return offsets;
}

// Enter: submit commands and complete C++; otherwise keep editing in the same
// buffer, including blank lines. Ctrl+C cancels an incomplete block.
Replxx::ACTION_RESULT on_enter(Replxx& rx, CppInterpreter& cpp,
                               const std::shared_ptr<SelectedSignatures>& selected,
                               SignatureCache& signature_cache, std::string& signature_cache_text,
                               char32_t code) {
  const auto state = rx.get_state();
  std::string text = state.text();
  std::size_t cursor = utf8_byte_offset(text, state.cursor_position());
  if (is_cpp_editor_payload(text, cursor) && !is_command_line(text)) {
    if (signature_cache_text != text) {
      signature_cache.clear();
      signature_cache_text = text;
    }
    const auto call_opens = completed_call_open_offsets(text);
    for (std::size_t call_open : call_opens) {
      auto call = editor_call_at_cursor(text, call_open + 1);
      if (!call || call->open != call_open || !call->has_close) {
        continue;
      }
      const bool explicit_empty_slot =
          call->arguments.size() > 1 &&
          std::any_of(call->arguments.begin(), call->arguments.end(),
                      [](const auto& argument) { return argument.begin == argument.end; });
      const std::string identity = selected_identity_for(*call, *selected);
      const auto cached = signature_cache.find(call_open);
      const bool has_cached_metadata =
          cached != signature_cache.end() && cached->second.identity == identity;
      if (!explicit_empty_slot && identity.empty() && !has_cached_metadata &&
          !is_trailing_call(text, *call)) {
        continue;
      }
      const std::size_t call_cursor =
          cursor > call->open && cursor <= call->close ? cursor : call->close;
      auto signatures = signatures_for_call(cpp, text, *call, call_cursor, *selected,
                                            signature_cache, has_cached_metadata);
      CppInterpreter::Completions completion;
      completion.signature_items = std::move(signatures);
      const auto* selected_item =
          enter_signature(completion, *call, selected_identity_for(*call, *selected));
      std::optional<CompletionItem> signature =
          selected_item == nullptr ? std::nullopt : std::optional<CompletionItem>(*selected_item);
      bool source_changed = false;
      while (signature && call->arguments.size() > 1) {
        const std::size_t last = call->arguments.size() - 1;
        if (last >= signature->parameters.size() || !signature->parameters[last].has_default ||
            call->arguments[last].begin != call->arguments[last].end) {
          break;
        }
        const std::size_t comma = call->arguments[last - 1].end;
        std::size_t erase_begin = comma;
        while (erase_begin > call->open + 1 &&
               (text[erase_begin - 1] == ' ' || text[erase_begin - 1] == '\t')) {
          --erase_begin;
        }
        if (comma >= call->close) {
          break;
        }
        const std::size_t removed = call->close - erase_begin;
        text.erase(erase_begin, removed);
        cursor = cursor > erase_begin ? std::max(erase_begin, cursor - removed) : cursor;
        source_changed = true;
        rx.set_state(Replxx::State(
            text.c_str(), static_cast<int>(utf8_length(std::string_view(text).substr(0, cursor)))));
        signature_cache.clear();
        signature_cache_text = text;
        call = editor_call_at_cursor(text, call_open + 1);
        if (!call || !call->has_close) {
          break;
        }
      }
      if (!call || !call->has_close) {
        continue;
      }
      if (source_changed && signature) {
        const std::size_t context =
            cursor > call->open && cursor <= call->close ? cursor : call->close;
        signature_cache[call_open] = {
            selected_identity_for(*call, *selected), context, {*signature}};
      }
      for (std::size_t i = 0; signature && i < signature->parameters.size(); ++i) {
        const auto& parameter = signature->parameters[i];
        if (parameter.is_pack) {
          continue;
        }
        bool later_argument_present = false;
        for (std::size_t later = i + 1; later < call->arguments.size(); ++later) {
          later_argument_present =
              later_argument_present || call->arguments[later].begin != call->arguments[later].end;
        }
        if (parameter.has_default && !later_argument_present) {
          continue; // Trailing optional parameters can keep their defaults.
        }
        std::size_t gap = call->close;
        bool append_separator = false;
        if (i < call->arguments.size()) {
          if (call->arguments[i].begin != call->arguments[i].end) {
            continue;
          }
          gap = call->arguments[i].begin;
        } else {
          append_separator = !call->arguments.empty() &&
                             call->arguments.back().begin != call->arguments.back().end;
        }
        rx.set_state(Replxx::State(
            text.c_str(), static_cast<int>(utf8_length(std::string_view(text).substr(0, gap)))));
        if (append_separator) {
          rx.invoke(Replxx::ACTION::INSERT_CHARACTER, ',');
          rx.invoke(Replxx::ACTION::INSERT_CHARACTER, ' ');
        }
        return Replxx::ACTION_RESULT::CONTINUE;
      }
    }
  }
  if (!is_cpp_editor_payload(text, cursor)) {
    return rx.invoke(
        wants_more_input(text) ? Replxx::ACTION::NEW_LINE : Replxx::ACTION::COMMIT_LINE, code);
  }
  if (!wants_more_input(text) && !editor_wants_more_input(text, cursor)) {
    return rx.invoke(Replxx::ACTION::COMMIT_LINE, code);
  }
  return rx.invoke(Replxx::ACTION::NEW_LINE, code);
}

Replxx::ACTION_RESULT on_delimiter(Replxx& rx, char32_t key) {
  const auto state = rx.get_state();
  const std::string text = state.text();
  const std::size_t cursor = utf8_byte_offset(text, state.cursor_position());
  const auto decision = delimiter_decision(text, cursor, key);
  switch (decision.action) {
  case DelimiterAction::InsertPair:
    rx.invoke(Replxx::ACTION::INSERT_CHARACTER, key);
    rx.invoke(Replxx::ACTION::INSERT_CHARACTER, decision.closing);
    return rx.invoke(Replxx::ACTION::MOVE_CURSOR_LEFT, 0);
  case DelimiterAction::SkipClosing:
    return rx.invoke(Replxx::ACTION::MOVE_CURSOR_RIGHT, 0);
  case DelimiterAction::DeletePair:
    rx.invoke(Replxx::ACTION::DELETE_CHARACTER_LEFT_OF_CURSOR, key);
    return rx.invoke(Replxx::ACTION::DELETE_CHARACTER_UNDER_CURSOR, key);
  case DelimiterAction::None:
    break;
  }
  if (key == '\b' || key == 127) {
    return rx.invoke(Replxx::ACTION::DELETE_CHARACTER_LEFT_OF_CURSOR, key);
  }
  return rx.invoke(Replxx::ACTION::INSERT_CHARACTER, key);
}

} // namespace

void configure(Replxx& rx, CppInterpreter& cpp) {
  rx.set_max_history_size(1000);
  rx.set_unique_history(true);
  rx.set_word_break_characters(" \t\n");
  auto selected_identity = std::make_shared<SelectedSignatures>();
  auto selected_text = std::make_shared<std::string>();
  auto signature_cache = std::make_shared<SignatureCache>();
  auto signature_cache_text = std::make_shared<std::string>();
  auto overload_menu = std::make_shared<ActiveOverloadMenu>();
  auto stale_overload_menu = std::make_shared<StaleOverloadMenu>();
  auto force_menu_repaint = [&rx](char32_t) {
    const auto state = rx.get_state();
    rx.set_state(Replxx::State(state.text(), state.cursor_position()));
  };
  auto menu_matches_current = [&rx](const OverloadMenu& menu) {
    const auto state = rx.get_state();
    const std::string text = state.text();
    return menu.source == text && menu.cursor == utf8_byte_offset(text, state.cursor_position());
  };
  auto identities = std::make_shared<std::unordered_map<std::string, std::string>>();
  rx.set_completion_callback([&cpp, &rx, identities](const std::string& input, int& context_len) {
    return complete(cpp, input, context_len, rx, *identities);
  });
  rx.set_completion_accept_callback([selected_identity, selected_text, signature_cache, identities,
                                     &rx](const std::string&, const std::string& display) {
    const auto state = rx.get_state();
    const auto buffer = state.text();
    sync_selected_signatures(selected_identity, selected_text, buffer);
    const auto cursor = utf8_byte_offset(buffer, state.cursor_position());
    auto call = editor_call_at_cursor(buffer, cursor);
    if (!call && cursor > 0) {
      call = editor_call_at_cursor(buffer, cursor - 1);
    }
    if (!call) {
      return;
    }
    const auto it = identities->find(display);
    if (it == identities->end()) {
      selected_identity->erase(call->open);
    } else {
      (*selected_identity)[call->open] = it->second;
    }
    signature_cache->erase(call->open);
  });
  rx.set_selection_menu_callback(
      [overload_menu, stale_overload_menu, &rx](const std::string& input) {
        std::vector<Replxx::SelectionMenuItem> rows;
        if (!*overload_menu) {
          return rows;
        }
        const auto state = rx.get_state();
        if ((*overload_menu)->source != input ||
            (*overload_menu)->cursor != utf8_byte_offset(input, state.cursor_position())) {
          stale_overload_menu->pending = ((*overload_menu)->source == input);
          stale_overload_menu->source = input;
          overload_menu->reset();
          return rows;
        }
        for (std::size_t i = 0; i < (*overload_menu)->items.size(); ++i) {
          rows.push_back({std::string(i == (*overload_menu)->selected ? "> " : "  ") +
                              overload_menu_label((*overload_menu)->items[i]),
                          i == (*overload_menu)->selected});
        }
        return rows;
      });
  rx.set_hint_callback([&cpp](const std::string& input, int& length, Replxx::Color& color) {
    return hint(cpp, input, length, color);
  });
  auto last_edit =
      std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
  auto last_input = std::make_shared<std::string>();
  auto committing = std::make_shared<bool>(false);
  auto seed_accepted_signature = [signature_cache, signature_cache_text, selected_text, last_input,
                                  last_edit](const std::string& source, std::size_t call_open,
                                             const CompletionItem& signature) {
    signature_cache->clear();
    *signature_cache_text = source;
    (*signature_cache)[call_open] = {signature.identity, call_open + 1, {signature}};
    *selected_text = source;
    *last_input = source;
    *last_edit = std::chrono::steady_clock::now() - std::chrono::milliseconds(200);
  };
  rx.set_inline_display_callback([&cpp, &rx, selected_identity, selected_text, last_edit,
                                  last_input, committing, signature_cache,
                                  signature_cache_text](const std::string&) {
    Replxx::inline_displays_t displays;
    const auto state = rx.get_state();
    const std::string full_input = state.text();
    if (*signature_cache_text != full_input) {
      signature_cache->clear();
      *signature_cache_text = full_input;
    }
    if (*committing && !cpp.options().keep_hints) {
      return displays;
    }
    sync_selected_signatures(selected_identity, selected_text, full_input);
    if (*last_input != full_input) {
      *last_input = full_input;
      *last_edit = std::chrono::steady_clock::now();
      if (!*committing) {
        return displays;
      }
    }
    if (!*committing &&
        std::chrono::steady_clock::now() - *last_edit < std::chrono::milliseconds(160)) {
      return displays;
    }
    if (is_command_line(full_input)) {
      return displays;
    }
    const auto cursor = utf8_byte_offset(full_input, state.cursor_position());
    const auto active = editor_call_at_cursor(full_input, cursor);
    auto frames = completed_calls(full_input);
    if (active) {
      const auto frame = std::find_if(frames.begin(), frames.end(), [&](const EditorCall& item) {
        return item.open == active->open;
      });
      if (frame == frames.end()) {
        frames.push_back(*active);
      } else {
        *frame = *active;
      }
    }
    for (const auto& frame : frames) {
      const bool is_active = active && active->open == frame.open;
      const std::size_t request_cursor = is_active ? std::min(cursor, frame.close) : frame.close;
      const auto signature = signature_for_call(cpp, full_input, frame, request_cursor,
                                                *selected_identity, *signature_cache);
      if (!signature) {
        continue;
      }
      if (signature->parameters.empty()) {
        if (!signature->detail.empty() && is_trailing_call(full_input, frame)) {
          const auto return_position = frame.has_close ? frame.close + 1 : frame.close;
          std::string annotation = " -> " + signature->detail;
          const auto qualifiers = callable_qualifiers(signature->label);
          if (!qualifiers.empty()) {
            annotation += " " + qualifiers;
          }
          displays.push_back({static_cast<int>(utf8_length(
                                  std::string_view(full_input).substr(0, return_position))),
                              std::move(annotation)});
        }
        continue;
      }
      for (std::size_t i = 0; i < signature->parameters.size(); ++i) {
        const auto& parameter = signature->parameters[i];
        const std::string prefix = parameter_label(parameter, i) + ": ";
        if (i < frame.arguments.size()) {
          const auto& argument = frame.arguments[i];
          int label_position =
              static_cast<int>(utf8_length(std::string_view(full_input).substr(0, argument.begin)));
          const bool active_argument = is_active && frame.active_argument == i;
          if (argument.begin == argument.end) {
            if (active_argument) {
              label_position =
                  static_cast<int>(utf8_length(std::string_view(full_input).substr(0, cursor)));
            }
            std::string ghost = prefix;
            if (!parameter.type.empty()) {
              ghost += " (" + parameter.type + ")";
            }
            const int cursor_offset = active_argument ? static_cast<int>(utf8_length(prefix)) : -1;
            displays.push_back({label_position, std::move(ghost), cursor_offset});
          } else {
            const int cursor_offset = active_argument ? static_cast<int>(utf8_length(prefix)) : -1;
            displays.push_back({label_position, prefix, cursor_offset});
            if (!parameter.type.empty()) {
              const auto type_position = static_cast<int>(
                  utf8_length(std::string_view(full_input).substr(0, argument.end)));
              displays.push_back({type_position, " (" + parameter.type + ")"});
            }
          }
        } else {
          std::string ghost = ", " + prefix;
          if (!parameter.type.empty()) {
            ghost += " (" + parameter.type + ")";
          }
          const auto ghost_position =
              static_cast<int>(utf8_length(std::string_view(full_input).substr(0, frame.close)));
          displays.push_back({ghost_position, std::move(ghost)});
        }
      }
      if (!signature->detail.empty() && is_trailing_call(full_input, frame)) {
        const auto return_position = frame.has_close ? frame.close + 1 : frame.close;
        std::string annotation = " -> " + signature->detail;
        const auto qualifiers = callable_qualifiers(signature->label);
        if (!qualifiers.empty()) {
          annotation += " " + qualifiers;
        }
        displays.push_back(
            {static_cast<int>(utf8_length(std::string_view(full_input).substr(0, return_position))),
             std::move(annotation)});
      }
    }
    std::stable_sort(displays.begin(), displays.end(), [](const auto& left, const auto& right) {
      return left.position < right.position;
    });
    return displays;
  });
  rx.set_highlighter_callback([last_edit, last_input, selected_identity, overload_menu,
                               stale_overload_menu,
                               selected_text](const std::string& input, Replxx::colors_t& colors) {
    sync_selected_signatures(selected_identity, selected_text, input);
    if (*overload_menu && (*overload_menu)->source != input) {
      overload_menu->reset();
      stale_overload_menu->pending = false;
    }
    if (*last_input != input) {
      stale_overload_menu->pending = false;
      *last_input = input;
      *last_edit = std::chrono::steady_clock::now();
    }
    highlight_input(input, colors);
  });
  rx.set_idle_highlighter_callback(
      [&cpp, last_edit, last_input](const std::string& input, Replxx::colors_t& colors) {
        const auto now = std::chrono::steady_clock::now();
        if (*last_input != input) {
          *last_input = input;
          *last_edit = now;
          return;
        }
        if (now - *last_edit < std::chrono::milliseconds(160)) {
          return;
        }
        highlight_editor_input(cpp, input, colors);
      });
  // Replxx invokes the idle highlighter and signature callback after input has
  // been quiet for this interval. Both can use the interpreter synchronously.
  rx.set_hint_delay(160);
  rx.set_max_hint_rows(4);

  // Custom binding: Ctrl+L clears the screen and redraws the prompt.
  rx.bind_key(Replxx::KEY::control('L'),
              [&rx](char32_t code) { return rx.invoke(Replxx::ACTION::CLEAR_SCREEN, code); });

  // Custom binding: Enter inserts a newline while the C++ input is incomplete.
  rx.bind_key(Replxx::KEY::ENTER, [&rx, &cpp, force_menu_repaint, menu_matches_current,
                                   stale_overload_menu, selected_identity, selected_text,
                                   signature_cache, signature_cache_text, overload_menu,
                                   seed_accepted_signature, committing](char32_t code) {
    if (stale_overload_menu->pending && rx.get_state().text() == stale_overload_menu->source) {
      stale_overload_menu->pending = false;
      overload_menu->reset();
      force_menu_repaint(code);
      return Replxx::ACTION_RESULT::CONTINUE;
    }
    // Input can change before the highlighter gets a chance to clear a stale
    // cursor-dismissed menu. Compare against the captured source here so a
    // rapid edit followed by Enter is handled as ordinary input.
    stale_overload_menu->pending = false;
    if (*overload_menu) {
      if (!menu_matches_current(**overload_menu)) {
        overload_menu->reset();
        force_menu_repaint(code);
        return Replxx::ACTION_RESULT::CONTINUE;
      }
      const auto& menu = **overload_menu;
      const auto state = rx.get_state();
      std::string source = state.text();
      sync_selected_signatures(selected_identity, selected_text, source);
      std::optional<CompletionItem> accepted_signature;
      std::size_t accepted_call_open = 0;
      if (menu.selected < menu.items.size()) {
        accepted_signature = menu.items[menu.selected];
        if (menu.from_prefix) {
          const auto& item = menu.items[menu.selected];
          source.replace(menu.prefix_begin, menu.cursor - menu.prefix_begin, item.text + "()");
          const std::size_t call_open = menu.prefix_begin + item.text.size();
          accepted_call_open = call_open;
          rx.set_state(Replxx::State(
              source.c_str(),
              static_cast<int>(utf8_length(std::string_view(source).substr(0, call_open + 1)))));
          sync_selected_signatures(selected_identity, selected_text, source);
          (*selected_identity)[call_open] = item.identity;
        } else {
          accepted_call_open = menu.call_open;
          (*selected_identity)[menu.call_open] = menu.items[menu.selected].identity;
        }
      }
      if (accepted_signature) {
        seed_accepted_signature(rx.get_state().text(), accepted_call_open, *accepted_signature);
      } else {
        *selected_text = rx.get_state().text();
      }
      overload_menu->reset();
      force_menu_repaint(code);
      return Replxx::ACTION_RESULT::CONTINUE;
    }
    struct CommitDisplayScope {
      bool& flag;
      explicit CommitDisplayScope(bool& value)
          : flag(value) {
        flag = true;
      }
      ~CommitDisplayScope() { flag = false; }
    } commit_display(*committing);
    overload_menu->reset();
    const auto result =
        on_enter(rx, cpp, selected_identity, *signature_cache, *signature_cache_text, code);
    const auto state = rx.get_state();
    const std::string current_text = state.text();
    if (current_text.empty() || result != Replxx::ACTION_RESULT::CONTINUE) {
      selected_identity->clear();
      *selected_text = current_text;
      signature_cache->clear();
      signature_cache_text->clear();
    }
    return result;
  });

  rx.bind_key(Replxx::KEY::control('C'),
              [&rx, selected_identity, selected_text, signature_cache, signature_cache_text,
               overload_menu, stale_overload_menu](char32_t code) {
                overload_menu->reset();
                stale_overload_menu->pending = false;
                selected_identity->clear();
                *selected_text = {};
                signature_cache->clear();
                signature_cache_text->clear();
                return rx.invoke(Replxx::ACTION::ABORT_LINE, code);
              });

  auto advance_snippet = [&cpp, &rx, selected_identity](bool backwards) {
    const auto state = rx.get_state();
    const std::string text = state.text();
    const auto cursor = utf8_byte_offset(text, state.cursor_position());
    const auto frame = editor_call_at_cursor(text, cursor);
    if (!frame) {
      return false;
    }
    const auto result = signatures_at_cursor(cpp, text, cursor);
    const auto* signature =
        selected_signature(result, selected_identity_for(*frame, *selected_identity));
    if (signature == nullptr || signature->parameters.empty()) {
      return false;
    }
    const std::size_t count = signature->parameters.size();
    const std::size_t current =
        std::min(static_cast<std::size_t>(frame->active_argument), count - 1);
    std::size_t target = frame->close;
    if (backwards) {
      if (current == 0) {
        return true;
      }
      target = frame->arguments[current - 1].begin;
    } else if (current + 1 < count) {
      if (current + 1 < frame->arguments.size()) {
        target = frame->arguments[current + 1].begin;
      } else {
        target = frame->arguments[current].end;
        rx.set_state(Replxx::State(
            text.c_str(), static_cast<int>(utf8_length(std::string_view(text).substr(0, target)))));
        rx.invoke(Replxx::ACTION::INSERT_CHARACTER, ',');
        rx.invoke(Replxx::ACTION::INSERT_CHARACTER, ' ');
        return true;
      }
    } else {
      target = frame->has_close ? frame->close + 1 : text.size();
    }
    rx.set_state(Replxx::State(
        text.c_str(), static_cast<int>(utf8_length(std::string_view(text).substr(0, target)))));
    return true;
  };
  auto accept_overload = [&rx, force_menu_repaint, menu_matches_current, overload_menu,
                          selected_identity, selected_text,
                          seed_accepted_signature](char32_t code) {
    if (!*overload_menu) {
      return false;
    }
    if (!menu_matches_current(**overload_menu)) {
      overload_menu->reset();
      force_menu_repaint(code);
      return false;
    }
    const auto& menu = **overload_menu;
    const auto state = rx.get_state();
    std::string source = state.text();
    sync_selected_signatures(selected_identity, selected_text, source);
    std::optional<CompletionItem> accepted_signature;
    std::size_t accepted_call_open = 0;
    if (menu.selected < menu.items.size()) {
      accepted_signature = menu.items[menu.selected];
      if (menu.from_prefix) {
        const auto& item = menu.items[menu.selected];
        const std::string inserted = item.text + "()";
        source.replace(menu.prefix_begin, menu.cursor - menu.prefix_begin, inserted);
        const std::size_t call_open = menu.prefix_begin + item.text.size();
        accepted_call_open = call_open;
        const std::size_t caret = call_open + 1;
        rx.set_state(Replxx::State(
            source.c_str(),
            static_cast<int>(utf8_length(std::string_view(source).substr(0, caret)))));
        sync_selected_signatures(selected_identity, selected_text, source);
        (*selected_identity)[call_open] = item.identity;
      } else {
        accepted_call_open = menu.call_open;
        (*selected_identity)[menu.call_open] = menu.items[menu.selected].identity;
      }
    }
    if (accepted_signature) {
      seed_accepted_signature(rx.get_state().text(), accepted_call_open, *accepted_signature);
    } else {
      *selected_text = rx.get_state().text();
    }
    overload_menu->reset();
    force_menu_repaint(code);
    return true;
  };
  auto move_overload = [force_menu_repaint, menu_matches_current, overload_menu](int direction,
                                                                                 char32_t code) {
    if (!*overload_menu || (*overload_menu)->items.empty()) {
      return false;
    }
    if (!menu_matches_current(**overload_menu)) {
      overload_menu->reset();
      force_menu_repaint(code);
      return false;
    }
    const std::size_t count = (*overload_menu)->items.size();
    if (direction > 0) {
      (*overload_menu)->selected = ((*overload_menu)->selected + 1) % count;
    } else {
      (*overload_menu)->selected = ((*overload_menu)->selected + count - 1) % count;
    }
    force_menu_repaint(code);
    return true;
  };
  rx.bind_key(Replxx::KEY::TAB, [&cpp, &rx, advance_snippet, force_menu_repaint, overload_menu,
                                 accept_overload, selected_identity,
                                 stale_overload_menu](char32_t code) {
    if (accept_overload(code)) {
      return Replxx::ACTION_RESULT::CONTINUE;
    }
    const auto state = rx.get_state();
    const std::string text = state.text();
    const auto cursor = utf8_byte_offset(text, state.cursor_position());
    const auto frame = editor_call_at_cursor(text, cursor);
    if (frame && should_complete_argument_prefix(cpp, text, *frame, cursor)) {
      return rx.invoke(Replxx::ACTION::COMPLETE_LINE, code);
    }
    if (open_overload_menu(cpp, text, cursor, *selected_identity, *overload_menu)) {
      stale_overload_menu->pending = false;
      force_menu_repaint(code);
      return Replxx::ACTION_RESULT::CONTINUE;
    }
    return advance_snippet(false) ? Replxx::ACTION_RESULT::CONTINUE
                                  : rx.invoke(Replxx::ACTION::COMPLETE_LINE, code);
  });
  rx.bind_key(Replxx::KEY::UP, [&rx, move_overload](char32_t code) {
    return move_overload(-1, code) ? Replxx::ACTION_RESULT::CONTINUE
                                   : rx.invoke(Replxx::ACTION::HISTORY_PREVIOUS, code);
  });
  rx.bind_key(Replxx::KEY::DOWN, [&rx, move_overload](char32_t code) {
    return move_overload(1, code) ? Replxx::ACTION_RESULT::CONTINUE
                                  : rx.invoke(Replxx::ACTION::HISTORY_NEXT, code);
  });
  rx.bind_key(Replxx::KEY::LEFT, [&rx, overload_menu, stale_overload_menu](char32_t code) {
    if (*overload_menu) {
      stale_overload_menu->pending = true;
      stale_overload_menu->source = rx.get_state().text();
    }
    overload_menu->reset();
    return rx.invoke(Replxx::ACTION::MOVE_CURSOR_LEFT, code);
  });
  rx.bind_key(Replxx::KEY::RIGHT, [&rx, overload_menu, stale_overload_menu](char32_t code) {
    if (*overload_menu) {
      stale_overload_menu->pending = true;
      stale_overload_menu->source = rx.get_state().text();
    }
    overload_menu->reset();
    return rx.invoke(Replxx::ACTION::MOVE_CURSOR_RIGHT, code);
  });
  rx.bind_key(Replxx::KEY::ESCAPE,
              [force_menu_repaint, overload_menu, stale_overload_menu](char32_t code) {
                if (*overload_menu) {
                  overload_menu->reset();
                  stale_overload_menu->pending = false;
                  force_menu_repaint(code);
                }
                return Replxx::ACTION_RESULT::CONTINUE;
              });
  rx.bind_key(Replxx::KEY::BASE_SHIFT | Replxx::KEY::TAB, [&rx, advance_snippet](char32_t code) {
    return advance_snippet(true) ? Replxx::ACTION_RESULT::CONTINUE
                                 : rx.invoke(Replxx::ACTION::COMPLETE_PREVIOUS, code);
  });

  for (const char32_t key : {'(', ')', '[', ']', '{', '}', '"', '\'', '<', '>'}) {
    rx.bind_key(key, [&rx, key](char32_t) { return on_delimiter(rx, key); });
  }
  rx.bind_key(Replxx::KEY::BACKSPACE + 0, [&rx](char32_t) { return on_delimiter(rx, '\b'); });
  rx.bind_key(127, [&rx](char32_t key) { return on_delimiter(rx, key); });
}

bool execute(LibraryManager& libs, CppInterpreter& cpp, const std::string& line) {
  const auto cmd = parse_command(line);
  if (cmd.kind == CommandKind::Cpp) {
    cpp.execute(line);
    return true;
  }
  return execute_command(libs, cpp, cmd, std::cout).keep_running;
}

} // namespace example
