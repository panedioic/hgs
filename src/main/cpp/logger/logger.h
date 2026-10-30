// logger/logger.h — a small printf-style logger with runtime-selectable sinks.
//
// The sinks implemented here are:
//   stderr — the process's own stderr (for test)
//   file   — write to a file in sandbox dir (path: LogDirectory, else
//            /data/storage/el2/base/files/logs/hgs-<pid>-<stamp>.log)
//   hilog  — HarmonyOS hilog (LOG_TAG "HGS_Logger")
//   custom — an optional callback registered with SetCustomWriter(), called
//            for every line alongside the sinks above
#pragma once

#include <cstdarg>
#include <functional>
#include <mutex>
#include <string>

#include "napi/native_api.h"  // 尾部 logger:: 命名空间的 NAPI 导出声明

class Logger {
public:
    static Logger* GetInstance();

    void LogI(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void LogW(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void LogE(const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    // 格式化核心 (public: 尾部 LOGI/LOGW/LOGE 内联转发用 — C 变参无法二次
    // 转发, 必须经 va_list)
    void LogV(int level, const char* fmt, va_list ap);

    // Passing an empty path leaves the current value unchanged.
    void SetLogFilePath(const std::string& path);
    std::string LogFilePath() const;

    // Custom sink: level 0/1/2 and the tagged line ("[I] ...").
    using CustomWriter = std::function<void(int level, const std::string& line)>;
    void SetCustomWriter(CustomWriter writer);

private:
    Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    mutable std::mutex mu_;
    // sink 开关 (默认全开; 无运行时 setter — 需要时在此翻转或补 setter)
    // level: 0 = Info, 1 = Warn, 2 = Error (see LogI/LogW/LogE)
    bool log_stderr_ = true;
    bool log_file_ = true;
    bool log_hilog_ = true;
    std::string file_path_;
    CustomWriter custom_writer_;
};

// C 变参无法直接转发给 LogI(fmt, ...) — 经公开的 LogV 用 va_list 转发
inline void LOGI(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Logger::GetInstance()->LogV(0, fmt, ap);
    va_end(ap);
}
inline void LOGW(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Logger::GetInstance()->LogV(1, fmt, ap);
    va_end(ap);
}
inline void LOGE(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Logger::GetInstance()->LogV(2, fmt, ap);
    va_end(ap);
}

namespace logger {

// NAPI: LOGI/LOGW/LOGE(msg: string) — ArkTS 侧日志入口, 单参数。
// tag 语义由调用方自行拼进消息 (如 "[CLICK-PIPE] ..."), 不单独传。
napi_value logiNAPI(napi_env env, napi_callback_info info);
napi_value logwNAPI(napi_env env, napi_callback_info info);
napi_value logeNAPI(napi_env env, napi_callback_info info);

} // namespace logger
