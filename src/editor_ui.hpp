#pragma once

#include "editor_analysis.hpp"
#include "replxx.hxx"

namespace example {
class CppInterpreter;
replxx::Replxx::Color semantic_color(SemanticKind kind);
void highlight_editor_input(CppInterpreter& cpp, const std::string& input,
                            replxx::Replxx::colors_t& colors);
} // namespace example
