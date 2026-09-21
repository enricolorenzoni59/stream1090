#pragma once
#include <iostream>
#include <sstream>
#include <mutex>
#include <string>
#include <chrono>
#include <ctime>
#include <iomanip>

namespace Log {

    enum class Level {
        ERROR = 0,
        WARN  = 1,
        MSG   = 2,
        INFO  = 3,
        DEBUG = 4
    };

    inline std::string short_timestamp() {
        using namespace std::chrono;

        auto now = system_clock::now();
        auto t = system_clock::to_time_t(now);
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tm{};
        ::localtime_r(&t, &tm);

        std::ostringstream oss;
        oss << std::put_time(&tm, "%H:%M:%S")
            << "." << std::setw(3) << std::setfill('0') << ms.count();
        return oss.str();
    }

    class Logger {
    public:
        static Logger& instance() {
            static Logger inst;
            return inst;
        }

        void setLevel(Level lvl) {
            std::lock_guard<std::mutex> lock(mutex_);
            max_level_ = lvl;
        }

        Level level() const {
            return max_level_;
        }

        void write(Level lvl, const std::string& src, const std::string& msg) {
            std::lock_guard<std::mutex> lock(mutex_);

            // New rule: print if lvl <= max_level_
            if (lvl > max_level_)
                return;

            std::string prefix = short_timestamp();
            if (!src.empty())
                prefix += " [" + src + "]";
            prefix += " ";

            if (msg.empty()) {
                std::cerr << prefix << "\n";
                return;
            }

            std::size_t start = 0;
            while (start < msg.size()) {
                std::size_t end = msg.find('\n', start);
                if (end == std::string::npos)
                    end = msg.size();
                if (end > start)
                    std::cerr << prefix << msg.substr(start, end - start) << "\n";
                start = end + 1;
            }
        }

    private:
        Logger() = default;
        mutable std::mutex mutex_;
        Level max_level_ = Level::MSG;   // default: normal messages + WARN + ERROR
    };


    class Stream {
    public:
        Stream(Level lvl, const std::string& src)
            : level_(lvl), source_(src) {}

        ~Stream() {
            Logger::instance().write(level_, source_, buffer_.str());
        }

        template<typename T>
        Stream& operator<<(const T& value) {
            buffer_ << value;
            return *this;
        }

    private:
        Level level_;
        std::string source_;
        std::ostringstream buffer_;
    };


    inline Stream error(const std::string& src) { return Stream(Level::ERROR, src); }
    inline Stream warn (const std::string& src) { return Stream(Level::WARN,  src); }
    inline Stream msg  (const std::string& src) { return Stream(Level::MSG,   src); }
    inline Stream info (const std::string& src) { return Stream(Level::INFO,  src); }
    inline Stream debug(const std::string& src) { return Stream(Level::DEBUG, src); }

    inline void error(const std::string& src, const std::string& msg) {
        Logger::instance().write(Level::ERROR, src, msg);
    }

    inline void warn(const std::string& src, const std::string& msg) {
        Logger::instance().write(Level::WARN, src, msg);
    }

    inline void msg(const std::string& src, const std::string& msg) {
        Logger::instance().write(Level::MSG, src, msg);
    }

    inline void info(const std::string& src, const std::string& msg) {
        Logger::instance().write(Level::INFO, src, msg);
    }

    inline void debug(const std::string& src, const std::string& msg) {
        Logger::instance().write(Level::DEBUG, src, msg);
    }

    inline void setLevel(Level lvl) {
        Logger::instance().setLevel(lvl);
    }

    inline Level getLevel() {
        return Logger::instance().level();
    }

} // namespace Log
