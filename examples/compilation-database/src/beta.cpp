#include <bonus.hpp>

static_assert(DEMO_VALUE == 22, "beta must use its own database entry");
static_assert(__cplusplus == 202002L, "beta must use C++20");

int beta_value() { return DEMO_VALUE + database_bonus; }
