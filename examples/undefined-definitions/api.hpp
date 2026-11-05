#pragma once

namespace definition_demo {
int adjust(int value);
extern int bias;
inline int initialization_count = 0;
inline int result = (++initialization_count, adjust(bias));
}  // namespace definition_demo
