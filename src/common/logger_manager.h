#ifndef LOGGER_MANAGER_H
#define LOGGER_MANAGER_H

#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <QtGlobal>
#include <QString>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/daily_file_sink.h>
#include "../util/convert_util.h"
#include "../util/str_util.h"

template <>
struct fmt::formatter<QString>
{
    constexpr auto parse(format_parse_context &ctx)
    {
        return ctx.begin();
    }

    template <typename FormatContext>
    auto format(const QString &q, FormatContext &ctx) const
    {
        return format_to(ctx.out(), "{}", q.toStdString());
    }
};

/* 从编译器函数签名中提取 "命名空间::类名::函数名" (去掉返回类型和形参) */
/* MSVC:  "int __cdecl ns::Class::func(int,char **)"  → "ns::Class::func" */
/* GCC:   "int ns::Class::func(int, char**)"           → "ns::Class::func" */
inline QString _extract_func_name(const char *sig)
{
    QString s = StrUtil::subBefore(QString::fromStdString(sig), "(");
    s = s.replace("__cdecl ", "");
    s = s.replace("__thiscall ", "");
    s = s.replace("auto ", "");
    return s;
}

/* 跨平台获取 "命名空间::函数名" (不含返回值和形参) */
#if defined(_MSC_VER)
#define CLASS_AND_FUNCTION() _extract_func_name(__FUNCSIG__)
#elif defined(__GNUC__) || defined(__clang__)
#define CLASS_AND_FUNCTION() _extract_func_name(__PRETTY_FUNCTION__)
#else
#define CLASS_AND_FUNCTION() _extract_func_name(__func__)
#endif

/* 辅助函数：QString转std::string，其它类型原样返回 */
template <typename T>
inline auto log_arg_cast(T &&arg) -> decltype(std::forward<T>(arg))
{
    return std::forward<T>(arg);
}
inline std::string log_arg_cast(const QString &arg)
{
    return arg.toStdString();
}
inline std::string log_arg_cast(QString &arg)
{
    return arg.toStdString();
}
inline std::string log_arg_cast(const QByteArray &arg)
{
    return QString::fromLocal8Bit(arg).toStdString();
}
inline std::string log_arg_cast(QByteArray &arg)
{
    return QString::fromLocal8Bit(arg).toStdString();
}

/* 递归转换所有参数为tuple */
template <typename... Args, std::size_t... I>
auto log_cast_tuple_impl(const std::tuple<Args...> &t, std::index_sequence<I...>)
{
    return std::make_tuple(log_arg_cast(std::get<I>(t))...);
}
template <typename... Args>
auto log_cast_tuple(Args &&...args)
{
    auto t = std::forward_as_tuple(std::forward<Args>(args)...);
    return log_cast_tuple_impl(t, std::index_sequence_for<Args...>{});
}

/* 展开tuple并传递给spdlog */
template <typename Tuple, typename F, std::size_t... I>
void log_apply(Tuple &&t, F &&f, std::index_sequence<I...>)
{
    f(std::get<I>(std::forward<Tuple>(t))...);
}
template <typename Tuple, typename F>
void log_apply(Tuple &&t, F &&f)
{
    constexpr auto size = std::tuple_size<std::decay_t<Tuple>>::value;
    log_apply(std::forward<Tuple>(t), std::forward<F>(f), std::make_index_sequence<size>{});
}

enum class LogSeverity
{
    Trace,
    Debug,
    Info,
    Warn,
    Error
};

template <LogSeverity Severity, typename LoggerPtr, typename Format>
class LogInvoker
{
public:
    LogInvoker(LoggerPtr logger, Format format)
        : m_logger(std::move(logger)), m_format(std::move(format))
    {
    }

    template <typename... Args>
    void operator()(Args &&...args) const
    {
        if constexpr (Severity == LogSeverity::Trace)
        {
            m_logger->trace(m_format, std::forward<Args>(args)...);
        }
        else if constexpr (Severity == LogSeverity::Debug)
        {
            m_logger->debug(m_format, std::forward<Args>(args)...);
        }
        else if constexpr (Severity == LogSeverity::Info)
        {
            m_logger->info(m_format, std::forward<Args>(args)...);
        }
        else if constexpr (Severity == LogSeverity::Warn)
        {
            m_logger->warn(m_format, std::forward<Args>(args)...);
        }
        else if constexpr (Severity == LogSeverity::Error)
        {
            m_logger->error(m_format, std::forward<Args>(args)...);
        }
    }

private:
    LoggerPtr m_logger;
    Format m_format;
};

template <LogSeverity Severity, typename LoggerPtr, typename Format>
auto makeLogInvoker(LoggerPtr &&logger, Format &&format)
{
    return LogInvoker<Severity, std::decay_t<LoggerPtr>, std::decay_t<Format>>(
        std::forward<LoggerPtr>(logger),
        std::forward<Format>(format));
}

class LoggerManager
{
public:
    static LoggerManager &instance();

    void initialize(const QString &logFilePath = "");
    std::shared_ptr<spdlog::logger> getLogger(const QString &name = "default");

    /* 便捷的日志宏 */
    template <typename... Args>
    void trace(const QString &fmt, Args &&...args)
    {
        auto tuple = log_cast_tuple(std::forward<Args>(args)...);
        log_apply(tuple, makeLogInvoker<LogSeverity::Trace>(instance().getLogger(), fmt.toStdString()));
    }
    template <typename... Args>
    void debug(const QString &fmt, Args &&...args)
    {
        auto tuple = log_cast_tuple(std::forward<Args>(args)...);
        log_apply(tuple, makeLogInvoker<LogSeverity::Debug>(instance().getLogger(), fmt.toStdString()));
    }

    template <typename... Args>
    void info(const QString &fmt, Args &&...args)
    {
        auto tuple = log_cast_tuple(std::forward<Args>(args)...);
        log_apply(tuple, makeLogInvoker<LogSeverity::Info>(instance().getLogger(), fmt.toStdString()));
    }

    template <typename... Args>
    void warn(const QString &fmt, Args &&...args)
    {
        auto tuple = log_cast_tuple(std::forward<Args>(args)...);
        log_apply(tuple, makeLogInvoker<LogSeverity::Warn>(instance().getLogger(), fmt.toStdString()));
    }

    template <typename... Args>
    void error(const QString &fmt, Args &&...args)
    {
        auto tuple = log_cast_tuple(std::forward<Args>(args)...);
        log_apply(tuple, makeLogInvoker<LogSeverity::Error>(instance().getLogger(), fmt.toStdString()));
    }

private:
    LoggerManager() = default;
    ~LoggerManager() = default;
    LoggerManager(const LoggerManager &) = delete;
    LoggerManager &operator=(const LoggerManager &) = delete;

    /* 私有帮助方法 */
    void setLogLevel(std::shared_ptr<spdlog::logger> logger) const;
    void setLogLevel(std::shared_ptr<spdlog::sinks::sink> sink) const;

    std::shared_ptr<spdlog::logger> m_logger;
    bool m_initialized = false;
    std::shared_ptr<spdlog::sinks::stdout_color_sink_mt> console_sink;
    std::shared_ptr<spdlog::sinks::daily_file_sink_mt> file_sink;
};

#define LOG_GENERIC(LOGGER_PTR, LEVEL, FMT, ...)                           \
    do                                                                     \
    {                                                                      \
        auto logger = (LOGGER_PTR);                                        \
        auto tuple = log_cast_tuple(__VA_ARGS__);                          \
        log_apply(tuple, makeLogInvoker<LogSeverity::LEVEL>(logger, FMT)); \
    } while (0)

/* 简化的日志宏定义 */
#define LOG_TRACE(fmt, ...) LOG_GENERIC(LoggerManager::instance().getLogger(CLASS_AND_FUNCTION()), Trace, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LOG_GENERIC(LoggerManager::instance().getLogger(CLASS_AND_FUNCTION()), Debug, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) LOG_GENERIC(LoggerManager::instance().getLogger(CLASS_AND_FUNCTION()), Info, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) LOG_GENERIC(LoggerManager::instance().getLogger(CLASS_AND_FUNCTION()), Warn, fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) LOG_GENERIC(LoggerManager::instance().getLogger(CLASS_AND_FUNCTION()), Warn, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG_GENERIC(LoggerManager::instance().getLogger(CLASS_AND_FUNCTION()), Error, fmt, ##__VA_ARGS__)
#endif /* LOGGER_MANAGER_H */
