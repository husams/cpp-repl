#include <bonus.hpp>

static_assert(DEMO_VALUE == 11, "alpha must use its own database entry");
static_assert(__cplusplus == 201703L, "alpha must use C++17");

int alpha_value() { return DEMO_VALUE + database_bonus; }
