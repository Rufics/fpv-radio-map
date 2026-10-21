#pragma once

// The "H name:value" lines that open every Blackbox log. They describe the frames completely
// (field names, signedness, predictor and encoding per frame type), so the decoder reads every
// firmware's frames from its own header instead of from a built-in field list.

#include "byte_reader.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fpv::blackbox {

/// A firmware version. Betaflight counts 3.1.0 … 4.5.x, then by year and month (2025.12.0).
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    auto operator<=>(const Version&) const = default;
    [[nodiscard]] std::string str() const;
    /// The first "N.N[.N]" in `text`; nullopt if there is none.
    static std::optional<Version> find(std::string_view text);
};

enum class FirmwareKind { Betaflight, Inav, Cleanflight, Emuflight, Other };

/// From "Firmware type", "Firmware revision", "Firmware date" and "Board information".
class FirmwareInfo {
public:
    static FirmwareInfo fromHeader(const std::map<std::string, std::string, std::less<>>& values);

    [[nodiscard]] FirmwareKind kind() const { return kind_; }
    [[nodiscard]] const std::string& name() const { return name_; }
    [[nodiscard]] const std::optional<Version>& version() const { return version_; }
    [[nodiscard]] const std::string& revision() const { return revision_; }
    [[nodiscard]] const std::string& target() const { return target_; }
    [[nodiscard]] const std::string& board() const { return board_; }
    [[nodiscard]] const std::string& buildDate() const { return buildDate_; }
    /// Betaflight at or above `v`.
    [[nodiscard]] bool betaflightAtLeast(const Version& v) const {
        return kind_ == FirmwareKind::Betaflight && version_ && *version_ >= v;
    }

private:
    FirmwareKind kind_ = FirmwareKind::Other;
    std::string name_ = "Unknown";
    std::optional<Version> version_;
    std::string revision_;
    std::string target_;
    std::string board_;
    std::string buildDate_;
};

struct FieldDefinition {
    std::string name;
    bool isSigned = false;
    int predictor = 0;
    int encoding = 0;
};

/// The fields of one frame type, in the order they are written.
class FrameDefinition {
public:
    void setNames(const std::vector<std::string>& names);
    void setSigned(const std::vector<long long>& flags);
    void setPredictors(const std::vector<long long>& predictors);
    void setEncodings(const std::vector<long long>& encodings);

    [[nodiscard]] std::size_t size() const { return fields_.size(); }
    [[nodiscard]] bool empty() const { return fields_.empty(); }
    [[nodiscard]] const FieldDefinition& field(std::size_t i) const { return fields_[i]; }
    [[nodiscard]] std::optional<std::size_t> indexOf(std::string_view name) const;
    /// Names, signedness, predictors and encodings all given for every field.
    [[nodiscard]] bool complete() const { return !fields_.empty() && predictorsSet_ && encodingsSet_; }

private:
    void resize(std::size_t n);
    std::vector<FieldDefinition> fields_;
    std::map<std::string, std::size_t, std::less<>> index_;
    bool predictorsSet_ = false;
    bool encodingsSet_ = false;
};

/// Which loop iterations the flight controller logs: an I frame every `I interval` iterations
/// and a P frame every `P interval` in between (Betaflight ≥ 3.1 writes "P interval:N"; older
/// firmware wrote "P interval:num/denom" and logged num of every denom iterations).
class FrameSchedule {
public:
    FrameSchedule() = default;
    FrameSchedule(std::uint32_t iInterval, std::uint32_t pNum, std::uint32_t pDenom);
    [[nodiscard]] bool logged(std::uint32_t iteration) const;
    /// Iterations after `last` that are not logged, up to the next logged one.
    [[nodiscard]] std::uint32_t skippedAfter(std::uint32_t last) const;

private:
    std::uint32_t iInterval_ = 32;
    std::uint32_t pNum_ = 1;
    std::uint32_t pDenom_ = 1;
};

class LogHeader {
public:
    /// Reads the header lines at the reader's position; stops at the first byte that does not
    /// start an "H " line. Lines without a colon are ignored.
    static LogHeader read(ByteReader& reader);

    [[nodiscard]] bool hasProduct() const { return values_.contains("Product"); }
    [[nodiscard]] std::size_t lineCount() const { return lines_; }
    [[nodiscard]] const FrameDefinition* frame(char type) const;
    [[nodiscard]] std::optional<std::string> value(std::string_view key) const;
    [[nodiscard]] std::optional<long long> integer(std::string_view key) const;
    /// A comma separated list of integers; empty if the key is missing.
    [[nodiscard]] std::vector<long long> integers(std::string_view key) const;
    [[nodiscard]] const std::map<std::string, std::string, std::less<>>& values() const { return values_; }

    [[nodiscard]] int dataVersion() const { return static_cast<int>(integer("Data version").value_or(1)); }
    [[nodiscard]] const FirmwareInfo& firmware() const { return firmware_; }
    [[nodiscard]] const FrameSchedule& schedule() const { return schedule_; }

    static std::vector<std::string> splitNames(std::string_view text);
    static std::vector<long long> splitIntegers(std::string_view text);

private:
    void addLine(std::string_view rawName, std::string_view rawValue);
    void addFieldLine(char type, std::string_view what, std::string_view value);
    void finish();

    std::map<std::string, std::string, std::less<>> values_;
    std::map<char, FrameDefinition> frames_;
    FirmwareInfo firmware_;
    FrameSchedule schedule_;
    std::size_t lines_ = 0;
};

}  // namespace fpv::blackbox
