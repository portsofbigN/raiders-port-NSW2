// Stand-in for Eden's common/logging.h in the standalone shader translator: messages go to stderr with fmt.
// The log class argument (Shader, Shader_SPIRV, ...) is only a label here.
#pragma once
#include <cstdio>
#include <string>
#include <utility>

#include <fmt/format.h>

namespace Common::Log {
template <typename... Args>
inline void Print(const char *level, const char *cls, fmt::format_string<Args...> f, Args &&...args) {
    std::string s = fmt::format(f, std::forward<Args>(args)...);
    std::fprintf(stderr, "[xlate %s %s] %s\n", cls, level, s.c_str());
}
}   // namespace Common::Log

#define LOG_TRACE(log_class, ...) (void(0))
#define LOG_DEBUG(log_class, ...) (void(0))
#define LOG_INFO(log_class, ...) ::Common::Log::Print("info", #log_class, __VA_ARGS__)
#define LOG_WARNING(log_class, ...) ::Common::Log::Print("warning", #log_class, __VA_ARGS__)
#define LOG_ERROR(log_class, ...) ::Common::Log::Print("error", #log_class, __VA_ARGS__)
#define LOG_CRITICAL(log_class, ...) ::Common::Log::Print("critical", #log_class, __VA_ARGS__)
