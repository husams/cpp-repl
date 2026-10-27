#include <prompt.hpp>

// This directory has no database entries; the chosen session reference applies.
static_assert(DEMO_VALUE == 22, "session defaults must come from beta.cpp");
static_assert(__cplusplus == 202002L, "session defaults must use C++20");

int default_beta_value() { return DEMO_VALUE + prompt_bonus; }
