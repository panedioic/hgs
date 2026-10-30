// logger/logger.cpp — see logger/logger.h.
#include "logger/logger.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "HGS_Logger"
#include <hilog/log.h>

namespace {

char LevelTag(int level) {
    if (level >= 2) return 'E';
    if (level == 1) return 'W';
    return 'I';
}

// The directory comes from the compile-time IEC_LOG_DIR when the platform
// defines it, otherwise from $IEC_LOG_DIR or $HOME/logs. The file name is
// assembled at runtime from the pid and a timestamp.
std::string LogDirectory() {
#ifdef IEC_LOG_DIR
    return IEC_LOG_DIR;
#else
    const char* dir = getenv("IEC_LOG_DIR");
    if (dir && *dir) return dir;
    const char* home = getenv("HOME");
    if (home && *home) return std::string(home) + "/logs";
    return std::string();
#endif
}

std::string DefaultLogPath() {
    std::string base = LogDirectory();
    if (base.empty()) base = "/data/storage/el2/base/files/logs";
    while (base.size() > 1 && base.back() == '/') base.pop_back();

    char stamp[32];
    const time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv) == 0) {
        std::snprintf(stamp, sizeof stamp, "%lu", (unsigned long)now);
    }

    return base + "/hgs-" + std::to_string((long)getpid()) + "-" + stamp + ".log";
}

}  // namespace

Logger::Logger() : file_path_(DefaultLogPath()) {}

Logger* Logger::GetInstance() {
    static Logger instance;
    return &instance;
}

void Logger::LogI(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(0, fmt, ap);
    va_end(ap);
}

void Logger::LogW(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(1, fmt, ap);
    va_end(ap);
}

void Logger::LogE(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(2, fmt, ap);
    va_end(ap);
}

void Logger::LogV(int level, const char* fmt, va_list ap) {
    if (!fmt) return;

    char buf[2048];
    vsnprintf(buf, sizeof buf, fmt, ap);
    const std::string tagged = std::string("[") + LevelTag(level) + "] " + buf;

    std::lock_guard<std::mutex> lk(mu_);

    // stderr
    if (log_stderr_) {
        fprintf(stderr, "%s\n", tagged.c_str());
        fflush(stderr);
    }

    // file
    if (log_file_) {
        const size_t slash = file_path_.find_last_of('/');
        if (slash != std::string::npos) {
            mkdir(file_path_.substr(0, slash).c_str(), 0755);  // EEXIST is fine
        }
        if (FILE* f = fopen(file_path_.c_str(), "a")) {
            fprintf(f, "%s\n", tagged.c_str());
            fclose(f);
        }
    }

    // hilog (级别由 hilog 自身承载, 只发正文不加 [I/W/E] 前缀)
    if (log_hilog_) {
        const LogLevel lvl = level >= 2 ? LOG_ERROR : (level == 1 ? LOG_WARN : LOG_INFO);
        OH_LOG_Print(LOG_APP, lvl, LOG_DOMAIN, LOG_TAG, "%{public}s", buf);
    }

    if (custom_writer_) {
        try {
            custom_writer_(level, tagged);
        } catch (...) {
            // A sink must not take the process down.
        }
    }
}

void Logger::SetCustomWriter(CustomWriter writer) {
    std::lock_guard<std::mutex> lk(mu_);
    custom_writer_ = std::move(writer);
}

void Logger::SetLogFilePath(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!path.empty()) file_path_ = path;
}

std::string Logger::LogFilePath() const {
    std::lock_guard<std::mutex> lk(mu_);
    return file_path_;
}

// ========================================================================
//  NAPI Export — ArkTS 侧日志入口 LOGI/LOGW/LOGE(tag, msg)
// ========================================================================

namespace logger {

namespace {

// 读 (tag, msg) 两个字符串参数; JS 数据不当 printf 格式串 (注入风险)
bool ReadTagMsg(napi_env env, napi_callback_info info, std::string& tag, std::string& msg) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) return false;
    size_t len = 0;
    if (napi_get_value_string_utf8(env, args[0], nullptr, 0, &len) != napi_ok) return false;
    tag.assign(len, '\0');
    if (napi_get_value_string_utf8(env, args[0], len ? &tag[0] : nullptr, len + 1, &len) != napi_ok) return false;
    if (napi_get_value_string_utf8(env, args[1], nullptr, 0, &len) != napi_ok) return false;
    msg.assign(len, '\0');
    if (napi_get_value_string_utf8(env, args[1], len ? &msg[0] : nullptr, len + 1, &len) != napi_ok) return false;
    return true;
}

}  // namespace

napi_value logiNAPI(napi_env env, napi_callback_info info) {
    std::string tag, msg;
    if (ReadTagMsg(env, info, tag, msg)) {
        Logger::GetInstance()->LogI("%s: %s", tag.c_str(), msg.c_str());
    }
    return nullptr;
}

napi_value logwNAPI(napi_env env, napi_callback_info info) {
    std::string tag, msg;
    if (ReadTagMsg(env, info, tag, msg)) {
        Logger::GetInstance()->LogW("%s: %s", tag.c_str(), msg.c_str());
    }
    return nullptr;
}

napi_value logeNAPI(napi_env env, napi_callback_info info) {
    std::string tag, msg;
    if (ReadTagMsg(env, info, tag, msg)) {
        Logger::GetInstance()->LogE("%s: %s", tag.c_str(), msg.c_str());
    }
    return nullptr;
}

}  // namespace logger
