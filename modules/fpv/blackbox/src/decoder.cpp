#include "decoder.hpp"

#include "frame_parser.hpp"
#include "log_header.hpp"

#include <algorithm>
#include <array>
#include <functional>

namespace fpv::blackbox {
namespace {

constexpr std::uint8_t kInvalid = 0xFF;

constexpr std::array<std::uint8_t, 256> base64Table() {
    std::array<std::uint8_t, 256> t{};
    t.fill(kInvalid);
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (std::size_t i = 0; i < alphabet.size(); ++i) t[static_cast<unsigned char>(alphabet[i])] = static_cast<std::uint8_t>(i);
    return t;
}

bool isSpace(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

}  // namespace

std::optional<std::vector<std::uint8_t>> Base64::decode(std::string_view text) {
    static constexpr auto kTable = base64Table();
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    std::size_t padding = 0;
    std::size_t symbols = 0;
    for (const char c : text) {
        if (isSpace(c)) continue;
        if (c == '=') {
            ++padding;
            ++symbols;
            continue;
        }
        const std::uint8_t v = kTable[static_cast<unsigned char>(c)];
        if (v == kInvalid || padding > 0) return std::nullopt;
        ++symbols;
        buffer = (buffer << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>(buffer >> bits));
            buffer &= (1u << bits) - 1u;
        }
    }
    if (padding > 2 || (padding > 0 && symbols % 4 != 0) || symbols % 4 == 1) return std::nullopt;
    return out;
}

BlackboxFile::BlackboxFile(std::span<const std::uint8_t> data) {
    const std::boyer_moore_horspool_searcher searcher(kProductLine.begin(), kProductLine.end());
    std::vector<std::size_t> starts;
    auto it = data.begin();
    while (true) {
        const auto [found, foundEnd] = searcher(it, data.end());
        if (found == data.end()) break;
        starts.push_back(static_cast<std::size_t>(found - data.begin()));
        it = foundEnd;
    }
    for (std::size_t i = 0; i < starts.size(); ++i) {
        if (logs_.size() == kMaxLogs) {
            truncated_ = true;
            break;
        }
        logs_.push_back({starts[i], i + 1 < starts.size() ? starts[i + 1] : data.size()});
    }
}

json BlackboxDecoder::probe(std::span<const std::uint8_t> data) const {
    const BlackboxFile file(data);
    json out = {{"format", kFormat}, {"decoder", moduleId_}, {"match", !file.logs().empty()}, {"logs", file.logs().size()}};
    if (!file.logs().empty()) {
        const auto& first = file.logs().front();
        ByteReader reader(data.subspan(first.begin, first.end - first.begin));
        const LogHeader header = LogHeader::read(reader);
        const FirmwareInfo& fw = header.firmware();
        const auto& version = fw.version();
        out["firmware"] = version ? fw.name() + " " + version->str() : fw.name();
    }
    return out;
}

json BlackboxDecoder::decodeLog(std::span<const std::uint8_t> data, const BlackboxFile::Segment& segment, int index,
                                const BuildOptions& options) {
    const auto bytes = data.subspan(segment.begin, segment.end - segment.begin);
    ByteReader reader(bytes);
    const LogHeader header = LogHeader::read(reader);
    NormalizedLogBuilder builder(header, options);
    FrameParser parser(header, bytes.subspan(reader.offset()));
    const ParseStats stats = parser.run(builder);
    return builder.finish(stats, index, {segment.begin, segment.end});
}

json BlackboxDecoder::decode(std::span<const std::uint8_t> data, std::string_view fileName, const BuildOptions& options) const {
    const BlackboxFile file(data);
    json source = {{"decoder", moduleId_}, {"decoderVersion", version_}, {"format", kFormat}, {"bytes", data.size()}};
    if (!fileName.empty()) source["fileName"] = std::string(fileName.substr(0, 255));
    json logs = json::array();
    int index = 0;
    for (const BlackboxFile::Segment& segment : file.logs()) logs.push_back(decodeLog(data, segment, ++index, options));
    return {{"schema", "flightlog/v1"}, {"source", std::move(source)}, {"logs", std::move(logs)}};
}

}  // namespace fpv::blackbox
