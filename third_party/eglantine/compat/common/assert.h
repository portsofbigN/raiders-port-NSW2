// Stand-in for Eden's common/assert.h in the standalone shader translator. A failed assertion throws instead of
// aborting, so the host can reject one shader without stopping the game.
#pragma once
#include <stdexcept>
#include <string>

#include <fmt/format.h>

namespace Common {
[[noreturn]] inline void AssertFailed(const char *file, int line, const std::string &msg) {
    throw std::logic_error(fmt::format("shader translator assertion at {}:{}: {}", file, line, msg));
}
}   // namespace Common

#define ASSERT_MSG(_a_, ...)                                                                                          \
    do {                                                                                                               \
        if (!(_a_)) ::Common::AssertFailed(__FILE__, __LINE__, fmt::format(__VA_ARGS__));                             \
    } while (0)
#define ASSERT(_a_) ASSERT_MSG(_a_, "{}", #_a_)
#define UNREACHABLE_MSG(...) ::Common::AssertFailed(__FILE__, __LINE__, fmt::format(__VA_ARGS__))
#define UNREACHABLE() ::Common::AssertFailed(__FILE__, __LINE__, "unreachable")
#define DEBUG_ASSERT(_a_) ASSERT(_a_)
#define DEBUG_ASSERT_MSG(_a_, ...) ASSERT_MSG(_a_, __VA_ARGS__)
#define UNIMPLEMENTED() ASSERT(false && "Unimplemented!")
#define UNIMPLEMENTED_MSG(...) ASSERT_MSG(false, __VA_ARGS__)
#define UNIMPLEMENTED_IF(cond) ASSERT((!(cond)) && "Unimplemented!")
#define UNIMPLEMENTED_IF_MSG(cond, ...) ASSERT_MSG(!(cond), __VA_ARGS__)
#define ASSERT_OR_EXECUTE_MSG(_a_, _b_, ...)                                                                          \
    do {                                                                                                               \
        if (!(_a_)) { _b_ }                                                                                            \
    } while (0)
#define ASSERT_OR_EXECUTE(_a_, _b_) ASSERT_OR_EXECUTE_MSG(_a_, _b_, "{}", #_a_)
