#include <prompt.hpp>

static_assert(DEMO_VALUE == 11, "switching the reference must replace beta's flags");
static_assert(__cplusplus == 201703L, "switching the reference must restore C++17");

int default_alpha_value() { return DEMO_VALUE + prompt_bonus; }
