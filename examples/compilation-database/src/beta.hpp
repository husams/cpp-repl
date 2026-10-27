#pragma once
#include <bonus.hpp>

// No database entry: beta.cpp wins because its filename stem matches.
static_assert(DEMO_VALUE == 22, "beta.hpp must borrow beta.cpp's options");
static_assert(__cplusplus == 202002L, "beta.hpp must borrow C++20");

inline int header_value() { return DEMO_VALUE + database_bonus + 1; }
