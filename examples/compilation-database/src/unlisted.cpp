#include <bonus.hpp>

// No database entry: alpha.cpp wins the same-directory alphabetical fallback.
static_assert(DEMO_VALUE == 11, "unlisted must borrow alpha's options");
static_assert(__cplusplus == 201703L, "unlisted must borrow C++17");

int unlisted_value() { return DEMO_VALUE + database_bonus + 1; }
