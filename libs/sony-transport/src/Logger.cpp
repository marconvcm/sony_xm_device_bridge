#include "sony/transport/Logger.h"

#include <atomic>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace sony {

namespace {

struct LoggerState {
    std::atomic<LogLevel> level{LogLevel::Info};
    std::atomic<bool> developerMode{false};
    std::mutex sinkMutex;
    Logger::LogSink customSink{nullptr};
};

LoggerState& state() {
    static LoggerState s_state;
    return s_state;
}

} // namespace

void Logger::setLogLevel(LogLevel level) noexcept {
    state().level.store(level, std::memory_order_relaxed);
}

LogLevel Logger::getLogLevel() noexcept {
    return state().level.load(std::memory_order_relaxed);
}

void Logger::setDeveloperMode(bool enabled) noexcept {
    state().developerMode.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        setLogLevel(LogLevel::Debug);
    }
}

bool Logger::isDeveloperMode() noexcept {
    return state().developerMode.load(std::memory_order_relaxed);
}

void Logger::setLogSink(LogSink sink) {
    std::lock_guard lock(state().sinkMutex);
    state().customSink = std::move(sink);
}

void Logger::resetLogSink() {
    std::lock_guard lock(state().sinkMutex);
    state().customSink = nullptr;
}

void Logger::log(LogLevel level, std::string_view category, std::string_view message) {
    if (level < getLogLevel()) {
        return;
    }

    LogSink sink;
    {
        std::lock_guard lock(state().sinkMutex);
        sink = state().customSink;
    }

    if (sink) {
        sink(level, category, message);
    } else {
        // Default output to stderr/clog
        std::clog << "[" << to_string(level) << "] [" << category << "] " << message << std::endl;
    }
}

void Logger::debug(std::string_view category, std::string_view message) {
    log(LogLevel::Debug, category, message);
}

void Logger::info(std::string_view category, std::string_view message) {
    log(LogLevel::Info, category, message);
}

void Logger::warn(std::string_view category, std::string_view message) {
    log(LogLevel::Warn, category, message);
}

void Logger::error(std::string_view category, std::string_view message) {
    log(LogLevel::Error, category, message);
}

std::string Logger::formatHex(std::span<const uint8_t> bytes, size_t maxBytes) {
    if (bytes.empty()) {
        return "";
    }
    std::ostringstream oss;
    size_t count = std::min(bytes.size(), maxBytes);
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) oss << " ";
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]);
    }
    if (bytes.size() > maxBytes) {
        oss << " ...";
    }
    return oss.str();
}

std::string Logger::describePayload(std::span<const uint8_t> payload) {
    if (payload.empty()) {
        return "";
    }

    uint8_t opcode = payload[0];
    switch (opcode) {
        case 0x22:
            return "BATTERY_GET";
        case 0x23: {
            if (payload.size() >= 4 && payload[1] == 0x00) {
                return "BATTERY_RET level=" + std::to_string(payload[2]);
            }
            if (payload.size() >= 6 && payload[1] == 0x09) {
                return "BATTERY_RET L=" + std::to_string(payload[2]) + " R=" + std::to_string(payload[4]);
            }
            if (payload.size() >= 4 && payload[1] == 0x0a) {
                return "BATTERY_RET Case=" + std::to_string(payload[2]);
            }
            return "BATTERY_RET";
        }
        case 0x25: {
            if (payload.size() >= 4 && payload[1] == 0x00) {
                return "BATTERY_NTFY level=" + std::to_string(payload[2]);
            }
            return "BATTERY_NTFY";
        }
        case 0x56:
            return "EQ_GET";
        case 0x57: {
            std::string s = "EQ_RET";
            if (payload.size() >= 3) {
                s += " preset=" + std::to_string(payload[2]);
            }
            return s;
        }
        case 0x58: {
            std::string s = "EQ_SET";
            if (payload.size() >= 3) {
                s += " preset=" + std::to_string(payload[2]);
            }
            return s;
        }
        case 0x66:
            return "NCASM_GET";
        case 0x67: {
            if (payload.size() >= 7 && (payload[1] == 0x17 || payload[1] == 0x19)) {
                bool on = (payload[3] != 0);
                bool ambient = (payload[4] != 0);
                int level = static_cast<int>(payload[6]);
                if (!on) return "NCASM_RET mode=Off";
                if (ambient) return "NCASM_RET mode=Ambient level=" + std::to_string(level);
                return "NCASM_RET mode=NoiseCancelling";
            }
            return "NCASM_RET";
        }
        case 0x68: {
            // NCASM_SET: payload[2] is setting type on v2 (0=NC, 1=Ambient)
            if (payload.size() >= 5) {
                if (payload[2] == 0x01) {
                    return "NCASM_SET mode=Ambient level=" + std::to_string(payload[4]);
                }
                return "NCASM_SET mode=NoiseCancelling";
            }
            return "NCASM_SET";
        }
        case 0xe6:
            return "DSEE_GET";
        case 0xe7:
            return (payload.size() >= 3 && payload[2] != 0) ? "DSEE_RET enabled=1" : "DSEE_RET enabled=0";
        case 0xe8:
            return (payload.size() >= 3 && payload[2] != 0) ? "DSEE_SET enabled=1" : "DSEE_SET enabled=0";
        default:
            break;
    }
    return "";
}

void Logger::logTx(std::span<const uint8_t> frameBytes, std::string_view semanticDesc) {
    std::string msg = "TX  " + formatHex(frameBytes);
    if (!semanticDesc.empty()) {
        msg += "\n" + std::string(semanticDesc);
    }
    debug(LogCategory::Session, msg);
}

void Logger::logRx(std::span<const uint8_t> frameBytes, std::string_view semanticDesc) {
    std::string msg = "RX  " + formatHex(frameBytes);
    if (!semanticDesc.empty()) {
        msg += "\n" + std::string(semanticDesc);
    }
    debug(LogCategory::Session, msg);
}

} // namespace sony
