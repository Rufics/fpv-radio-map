#include "log_header.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <format>

namespace fpv::blackbox {
namespace {

constexpr std::size_t kMaxHeaderLine = 4096;
constexpr std::size_t kMaxFields = 256;

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

std::optional<long long> parseInteger(std::string_view text) {
    text = trim(text);
    long long value = 0;
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value, base);
    if (ec != std::errc() || ptr == text.data()) return std::nullopt;
    return value;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

}  // namespace

std::string Version::str() const { return std::format("{}.{}.{}", major, minor, patch); }

std::optional<Version> Version::find(std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i])) || (i > 0 && std::isalnum(static_cast<unsigned char>(text[i - 1])))) {
            continue;
        }
        int parts[3] = {0, 0, 0};
        std::size_t pos = i;
        int count = 0;
        while (count < 3 && pos < text.size()) {
            const auto [ptr, ec] = std::from_chars(text.data() + pos, text.data() + text.size(), parts[count]);
            if (ec != std::errc()) break;
            ++count;
            pos = static_cast<std::size_t>(ptr - text.data());
            if (pos >= text.size() || text[pos] != '.') break;
            ++pos;
        }
        if (count >= 2) return Version{parts[0], parts[1], parts[2]};
    }
    return std::nullopt;
}

FirmwareInfo FirmwareInfo::fromHeader(const std::map<std::string, std::string, std::less<>>& values) {
    FirmwareInfo info;
    const auto get = [&values](std::string_view key) -> std::string {
        const auto it = values.find(key);
        return it == values.end() ? std::string() : it->second;
    };
    const std::string revision = get("Firmware revision");
    const std::string type = get("Firmware type");
    info.board_ = std::string(trim(get("Board information")));
    info.buildDate_ = std::string(trim(get("Firmware date")));

    // "Betaflight 4.5.1 (77d01ba3b) STM32F7X2", "INAV 7.1.0 (a1b2c3d) MATEKF405", or only a git
    // hash on early Cleanflight.
    const std::string_view rev = trim(revision);
    const std::string_view first = rev.substr(0, rev.find(' '));
    if (equalsIgnoreCase(first, "Betaflight")) {
        info.kind_ = FirmwareKind::Betaflight;
        info.name_ = "Betaflight";
    } else if (equalsIgnoreCase(first, "INAV")) {
        info.kind_ = FirmwareKind::Inav;
        info.name_ = "INAV";
    } else if (equalsIgnoreCase(first, "EmuFlight")) {
        info.kind_ = FirmwareKind::Emuflight;
        info.name_ = "EmuFlight";
    } else if (equalsIgnoreCase(first, "Cleanflight") || equalsIgnoreCase(trim(type), "Cleanflight")) {
        info.kind_ = FirmwareKind::Cleanflight;
        info.name_ = "Cleanflight";
    } else if (!trim(type).empty()) {
        info.name_ = std::string(trim(type));
    }
    if (info.kind_ != FirmwareKind::Cleanflight || equalsIgnoreCase(first, "Cleanflight")) {
        info.version_ = Version::find(rev.substr(first.size()));
    }
    if (const auto open = rev.find('('); open != std::string_view::npos) {
        if (const auto close = rev.find(')', open); close != std::string_view::npos) {
            info.revision_ = std::string(trim(rev.substr(open + 1, close - open - 1)));
            info.target_ = std::string(trim(rev.substr(close + 1)));
        }
    } else if (info.kind_ == FirmwareKind::Cleanflight && !equalsIgnoreCase(first, "Cleanflight")) {
        info.revision_ = std::string(first);
    }
    return info;
}

void FrameDefinition::resize(std::size_t n) {
    if (n > kMaxFields) n = kMaxFields;
    if (fields_.size() < n) fields_.resize(n);
}

void FrameDefinition::setNames(const std::vector<std::string>& names) {
    fields_.clear();
    index_.clear();
    resize(names.size());
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        fields_[i].name = names[i];
        index_.emplace(names[i], i);
    }
}

void FrameDefinition::setSigned(const std::vector<long long>& flags) {
    for (std::size_t i = 0; i < fields_.size() && i < flags.size(); ++i) fields_[i].isSigned = flags[i] != 0;
}

void FrameDefinition::setPredictors(const std::vector<long long>& predictors) {
    for (std::size_t i = 0; i < fields_.size() && i < predictors.size(); ++i) {
        fields_[i].predictor = static_cast<int>(predictors[i]);
    }
    predictorsSet_ = !fields_.empty() && predictors.size() >= fields_.size();
}

void FrameDefinition::setEncodings(const std::vector<long long>& encodings) {
    for (std::size_t i = 0; i < fields_.size() && i < encodings.size(); ++i) {
        fields_[i].encoding = static_cast<int>(encodings[i]);
    }
    encodingsSet_ = !fields_.empty() && encodings.size() >= fields_.size();
}

std::optional<std::size_t> FrameDefinition::indexOf(std::string_view name) const {
    const auto it = index_.find(name);
    if (it == index_.end()) return std::nullopt;
    return it->second;
}

FrameSchedule::FrameSchedule(std::uint32_t iInterval, std::uint32_t pNum, std::uint32_t pDenom)
    : iInterval_(std::max<std::uint32_t>(iInterval, 1)),
      pNum_(std::max<std::uint32_t>(pNum, 1)),
      pDenom_(std::max<std::uint32_t>(pDenom, 1)) {}

bool FrameSchedule::logged(std::uint32_t iteration) const {
    return ((iteration % iInterval_) + pNum_ - 1) % pDenom_ < pNum_;
}

std::uint32_t FrameSchedule::skippedAfter(std::uint32_t last) const {
    std::uint32_t skipped = 0;
    // The pattern repeats every I interval, so the next logged iteration is never further away.
    for (std::uint32_t i = last + 1; !logged(i) && skipped < iInterval_; ++i) ++skipped;
    return skipped;
}

LogHeader LogHeader::read(ByteReader& reader) {
    LogHeader header;
    while (reader.lookingAt("H ")) {
        const std::size_t start = reader.offset();
        std::string line;
        reader.readByte();
        reader.readByte();
        while (!reader.atEnd() && reader.peek() != '\n' && line.size() < kMaxHeaderLine) {
            line.push_back(static_cast<char>(reader.readByte()));
        }
        if (reader.peek() != '\n') {
            reader.seek(start);  // a line that never ends is not header any more
            break;
        }
        reader.readByte();
        ++header.lines_;
        if (const auto colon = line.find(':'); colon != std::string::npos) {
            header.addLine(trim(std::string_view(line).substr(0, colon)), std::string_view(line).substr(colon + 1));
        }
    }
    header.finish();
    return header;
}

void LogHeader::addLine(std::string_view name, std::string_view value) {
    // "Field I name", "Field P predictor", ...
    if (name.size() > 8 && name.starts_with("Field ") && name[7] == ' ') {
        addFieldLine(name[6], name.substr(8), value);
        return;
    }
    values_[std::string(name)] = std::string(value);
}

void LogHeader::addFieldLine(char type, std::string_view what, std::string_view value) {
    FrameDefinition& def = frames_[type];
    if (what == "name") {
        def.setNames(splitNames(value));
        // P frames carry the I frame's fields with their own predictors and encodings.
        if (type == 'I') frames_['P'].setNames(splitNames(value));
    } else if (what == "signed") {
        def.setSigned(splitIntegers(value));
        if (type == 'I') frames_['P'].setSigned(splitIntegers(value));
    } else if (what == "predictor") {
        def.setPredictors(splitIntegers(value));
    } else if (what == "encoding") {
        def.setEncodings(splitIntegers(value));
    }
}

void LogHeader::finish() {
    firmware_ = FirmwareInfo::fromHeader(values_);
    const auto iInterval = integer("I interval").value_or(32);
    std::uint32_t num = 1;
    std::uint32_t denom = 1;
    if (const auto p = value("P interval")) {
        if (const auto slash = p->find('/'); slash != std::string::npos) {
            num = static_cast<std::uint32_t>(std::max(1LL, parseInteger(std::string_view(*p).substr(0, slash)).value_or(1)));
            denom = static_cast<std::uint32_t>(std::max(1LL, parseInteger(std::string_view(*p).substr(slash + 1)).value_or(1)));
        } else {
            denom = static_cast<std::uint32_t>(std::max(1LL, parseInteger(*p).value_or(1)));
        }
    }
    schedule_ = FrameSchedule(static_cast<std::uint32_t>(std::clamp(iInterval, 1LL, 65535LL)), num, denom);
}

const FrameDefinition* LogHeader::frame(char type) const {
    const auto it = frames_.find(type);
    return it == frames_.end() || it->second.empty() ? nullptr : &it->second;
}

std::optional<std::string> LogHeader::value(std::string_view key) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

std::optional<long long> LogHeader::integer(std::string_view key) const {
    const auto v = value(key);
    if (!v) return std::nullopt;
    return parseInteger(*v);
}

std::vector<long long> LogHeader::integers(std::string_view key) const {
    const auto v = value(key);
    return v ? splitIntegers(*v) : std::vector<long long>{};
}

std::vector<std::string> LogHeader::splitNames(std::string_view text) {
    std::vector<std::string> out;
    while (!text.empty() && out.size() < kMaxFields) {
        const auto comma = text.find(',');
        out.emplace_back(trim(text.substr(0, comma)));
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    return out;
}

std::vector<long long> LogHeader::splitIntegers(std::string_view text) {
    std::vector<long long> out;
    for (const std::string& part : splitNames(text)) out.push_back(parseInteger(part).value_or(0));
    return out;
}

}  // namespace fpv::blackbox
