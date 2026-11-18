// Crash Course 1 shader pack: rebuilds the runtime shaders the game asks for from the game's own files and the
// deltas table.

#include "cc1_pack.h"

#include "shader.h"
#include "ucode_fingerprint.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace
{

constexpr char kDeltasHeader[] = "runtime_ucode_hash,type,translate,source,source_hash,words,patches";
constexpr char kPakMagic[] = "tongas_pack_v30000";
constexpr size_t kPakMagicSize = sizeof(kPakMagic) - 1;

std::vector<uint8_t> readFile(const std::filesystem::path& path)
{
    FILE* file = fopen(path.string().c_str(), "rb");
    if (file == nullptr)
        throw std::runtime_error("cannot open " + path.string());
    fseek(file, 0, SEEK_END);
    std::vector<uint8_t> data(size_t(ftell(file)));
    fseek(file, 0, SEEK_SET);
    const size_t read = fread(data.data(), 1, data.size(), file);
    fclose(file);
    if (read != data.size())
        throw std::runtime_error("cannot read " + path.string());
    return data;
}

void writeFile(const std::filesystem::path& path, const void* data, size_t size)
{
    FILE* file = fopen(path.string().c_str(), "wb");
    if (file == nullptr)
        throw std::runtime_error("cannot write " + path.string());
    const size_t written = fwrite(data, 1, size, file);
    fclose(file);
    if (written != size)
        throw std::runtime_error("cannot write " + path.string());
}

uint32_t readLe32(const uint8_t* data)
{
    return uint32_t(data[0]) | (uint32_t(data[1]) << 8) | (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}

uint32_t readBe32(const uint8_t* data)
{
    return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) | (uint32_t(data[2]) << 8) | uint32_t(data[3]);
}

void writeBe32(uint8_t* data, uint32_t value)
{
    data[0] = uint8_t(value >> 24);
    data[1] = uint8_t(value >> 16);
    data[2] = uint8_t(value >> 8);
    data[3] = uint8_t(value);
}

// Decoder for the .lzma container the game's pack files store their entries in.
class LzmaDecoder
{
public:
    LzmaDecoder(const uint8_t* data, size_t size) : in(data), end(data + size)
    {
        const auto reset = [](auto& probs)
            {
                std::fill_n(reinterpret_cast<uint16_t*>(&probs), sizeof(probs) / sizeof(uint16_t), kInitial);
            };
        reset(isMatch);
        reset(isRep);
        reset(isRepG0);
        reset(isRepG1);
        reset(isRepG2);
        reset(isRep0Long);
        reset(posSlot);
        reset(posSpecial);
        reset(posAlign);
    }

    std::vector<uint8_t> decode()
    {
        if (end - in < 13)
            throw std::runtime_error("LZMA stream is truncated");
        uint32_t properties = in[0];
        if (properties >= 9 * 5 * 5)
            throw std::runtime_error("LZMA properties are invalid");
        const uint32_t lc = properties % 9;
        properties /= 9;
        const uint32_t lp = properties % 5;
        const uint32_t pb = properties / 5;
        uint64_t unpackedSize = 0;
        for (uint32_t i = 0; i < 8; i++)
            unpackedSize |= uint64_t(in[5 + i]) << (8 * i);
        const bool sized = unpackedSize != UINT64_MAX;
        if (sized && unpackedSize > (1ull << 30))
            throw std::runtime_error("LZMA stream is larger than a pack entry can be");
        in += 13;

        if (next() != 0)
            throw std::runtime_error("LZMA stream is invalid");
        for (uint32_t i = 0; i < 4; i++)
            code = (code << 8) | next();

        std::vector<uint16_t> literal(size_t(0x300) << (lc + lp), kInitial);
        std::vector<uint8_t> out;
        if (sized)
            out.reserve(size_t(unpackedSize));

        uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
        uint32_t state = 0;
        while (!sized || out.size() < unpackedSize)
        {
            const uint32_t posState = uint32_t(out.size()) & ((1u << pb) - 1);
            if (bit(isMatch[(state << 4) + posState]) == 0)
            {
                const uint32_t previous = out.empty() ? 0 : out.back();
                const uint32_t context = ((uint32_t(out.size()) & ((1u << lp) - 1)) << lc) + (previous >> (8 - lc));
                uint16_t* probs = &literal[size_t(0x300) * context];
                uint32_t symbol = 1;
                if (state >= 7)
                {
                    uint32_t matchByte = out[out.size() - rep0 - 1];
                    do
                    {
                        const uint32_t matchBit = (matchByte >> 7) & 1;
                        matchByte <<= 1;
                        const uint32_t decoded = bit(probs[((1 + matchBit) << 8) + symbol]);
                        symbol = (symbol << 1) | decoded;
                        if (matchBit != decoded)
                            break;
                    } while (symbol < 0x100);
                }
                while (symbol < 0x100)
                    symbol = (symbol << 1) | bit(probs[symbol]);
                out.push_back(uint8_t(symbol));
                state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
                continue;
            }

            uint32_t length;
            if (bit(isRep[state]) != 0)
            {
                if (out.empty())
                    throw std::runtime_error("LZMA stream is invalid");
                if (bit(isRepG0[state]) == 0)
                {
                    if (bit(isRep0Long[(state << 4) + posState]) == 0)
                    {
                        state = state < 7 ? 9 : 11;
                        out.push_back(out[out.size() - rep0 - 1]);
                        continue;
                    }
                }
                else
                {
                    uint32_t distance;
                    if (bit(isRepG1[state]) == 0)
                    {
                        distance = rep1;
                    }
                    else
                    {
                        if (bit(isRepG2[state]) == 0)
                        {
                            distance = rep2;
                        }
                        else
                        {
                            distance = rep3;
                            rep3 = rep2;
                        }
                        rep2 = rep1;
                    }
                    rep1 = rep0;
                    rep0 = distance;
                }
                length = decodeLength(repLength, posState);
                state = state < 7 ? 8 : 11;
            }
            else
            {
                rep3 = rep2;
                rep2 = rep1;
                rep1 = rep0;
                length = decodeLength(matchLength, posState);
                state = state < 7 ? 7 : 10;
                rep0 = decodeDistance(length);
                // The all-ones distance is the end marker.
                if (rep0 == 0xFFFFFFFF)
                    break;
            }

            length += 2;
            if (rep0 >= out.size() || (sized && out.size() + length > unpackedSize))
                throw std::runtime_error("LZMA stream is invalid");
            for (uint32_t i = 0; i < length; i++)
                out.push_back(out[out.size() - rep0 - 1]);
        }

        if (sized && out.size() != unpackedSize)
            throw std::runtime_error("LZMA stream ends early");
        return out;
    }

private:
    static constexpr uint16_t kInitial = 1024;

    struct LengthCoder
    {
        uint16_t choice = kInitial;
        uint16_t choice2 = kInitial;
        uint16_t low[16][8];
        uint16_t mid[16][8];
        uint16_t high[256];

        LengthCoder()
        {
            std::fill_n(&low[0][0], 16 * 8, kInitial);
            std::fill_n(&mid[0][0], 16 * 8, kInitial);
            std::fill_n(high, 256, kInitial);
        }
    };

    uint8_t next()
    {
        if (in == end)
            throw std::runtime_error("LZMA stream is truncated");
        return *in++;
    }

    void normalize()
    {
        if (range < (1u << 24))
        {
            range <<= 8;
            code = (code << 8) | next();
        }
    }

    uint32_t bit(uint16_t& prob)
    {
        const uint32_t bound = (range >> 11) * prob;
        uint32_t result;
        if (code < bound)
        {
            range = bound;
            prob += (2048 - prob) >> 5;
            result = 0;
        }
        else
        {
            range -= bound;
            code -= bound;
            prob -= prob >> 5;
            result = 1;
        }
        normalize();
        return result;
    }

    uint32_t directBits(uint32_t count)
    {
        uint32_t result = 0;
        for (; count != 0; count--)
        {
            range >>= 1;
            code -= range;
            const uint32_t borrow = 0u - (code >> 31);
            code += range & borrow;
            normalize();
            result = (result << 1) + borrow + 1;
        }
        return result;
    }

    uint32_t tree(uint16_t* probs, uint32_t bits)
    {
        uint32_t node = 1;
        for (uint32_t i = 0; i < bits; i++)
            node = (node << 1) + bit(probs[node]);
        return node - (1u << bits);
    }

    uint32_t reverseTree(uint16_t* probs, uint32_t bits)
    {
        uint32_t node = 1;
        uint32_t symbol = 0;
        for (uint32_t i = 0; i < bits; i++)
        {
            const uint32_t decoded = bit(probs[node]);
            node = (node << 1) + decoded;
            symbol |= decoded << i;
        }
        return symbol;
    }

    uint32_t decodeLength(LengthCoder& coder, uint32_t posState)
    {
        if (bit(coder.choice) == 0)
            return tree(coder.low[posState], 3);
        if (bit(coder.choice2) == 0)
            return 8 + tree(coder.mid[posState], 3);
        return 16 + tree(coder.high, 8);
    }

    uint32_t decodeDistance(uint32_t length)
    {
        const uint32_t slot = tree(posSlot[length > 3 ? 3 : length], 6);
        if (slot < 4)
            return slot;
        const uint32_t bits = (slot >> 1) - 1;
        uint32_t distance = (2 | (slot & 1)) << bits;
        if (slot < 14)
        {
            distance += reverseTree(posSpecial + distance - slot, bits);
        }
        else
        {
            distance += directBits(bits - 4) << 4;
            distance += reverseTree(posAlign, 4);
        }
        return distance;
    }

    const uint8_t* in;
    const uint8_t* end;
    uint32_t range = 0xFFFFFFFF;
    uint32_t code = 0;
    uint16_t isMatch[12 << 4];
    uint16_t isRep[12];
    uint16_t isRepG0[12];
    uint16_t isRepG1[12];
    uint16_t isRepG2[12];
    uint16_t isRep0Long[12 << 4];
    uint16_t posSlot[4][64];
    uint16_t posSpecial[115];
    uint16_t posAlign[16];
    LengthCoder matchLength;
    LengthCoder repLength;
};

// A byte range the scan covers: a whole file, or one decompressed entry of a pack file.
struct Range
{
    std::string name;
    std::vector<uint8_t> bytes;
};

struct CodeSpan
{
    const uint8_t* data = nullptr;
    size_t size = 0;
};

CodeSpan containerCode(const uint8_t* base)
{
    auto container = reinterpret_cast<const ShaderContainer*>(base);
    auto shader = reinterpret_cast<const Shader*>(base + container->shaderOffset);
    return { base + container->virtualSize + shader->physicalOffset, shader->size };
}

bool containerIsPixelShader(const uint8_t* base)
{
    return (reinterpret_cast<const ShaderContainer*>(base)->flags & 0x1) == 0;
}

uint64_t fingerprintOf(const uint8_t* code, size_t size)
{
    // The walk reads whole words, so unaligned image bytes are copied first.
    std::vector<uint32_t> words(size / sizeof(uint32_t));
    memcpy(words.data(), code, words.size() * sizeof(uint32_t));
    return ucodeFingerprint(words.data(), words.size(), true);
}

bool endsWithNoCase(std::string_view text, std::string_view suffix)
{
    if (text.size() < suffix.size())
        return false;
    for (size_t i = 0; i < suffix.size(); i++)
    {
        if (tolower(uint8_t(text[text.size() - suffix.size() + i])) != suffix[i])
            return false;
    }
    return true;
}

// The game image, the shader precache and the title update's pack, indexed for lookups.
struct Corpus
{
    std::vector<Range> ranges;
    std::map<uint64_t, const uint8_t*> containers; // XXH3 of the container bytes

    void addFile(const std::filesystem::path& path)
    {
        std::vector<uint8_t> data = readFile(path);
        const std::string name = path.filename().string();
        if (data.size() < kPakMagicSize + 8 || memcmp(data.data(), kPakMagic, kPakMagicSize) != 0)
        {
            ranges.push_back({ name, std::move(data) });
            return;
        }

        // Pack index: count, index size, then per entry offset, size, flag, name length and name.
        const uint32_t count = readLe32(data.data() + kPakMagicSize);
        size_t at = kPakMagicSize + 8;
        size_t precaches = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            if (at + 16 > data.size())
                throw std::runtime_error(name + " has a truncated index");
            const uint32_t offset = readLe32(data.data() + at);
            const uint32_t size = readLe32(data.data() + at + 4);
            const uint32_t flag = readLe32(data.data() + at + 8);
            const uint32_t nameSize = readLe32(data.data() + at + 12);
            at += 16;
            if (nameSize > data.size() - at)
                throw std::runtime_error(name + " has a truncated index");
            std::string entry(reinterpret_cast<const char*>(data.data() + at), nameSize);
            at += nameSize;
            entry.resize(strlen(entry.c_str()));
            if (!endsWithNoCase(entry, ".bsh"))
                continue;
            if (offset > data.size() || size > data.size() - offset)
                throw std::runtime_error(name + ": " + entry + " runs past the end of the pack");
            // Flag 1 marks an LZMA entry, anything else is stored as is.
            ranges.push_back({ name + ":" + entry, flag == 1
                ? LzmaDecoder(data.data() + offset, size).decode()
                : std::vector<uint8_t>(data.begin() + offset, data.begin() + offset + size) });
            precaches++;
        }
        if (precaches == 0)
            fmt::println("[cc1] {} holds no shader precache", name);
    }

    void scan()
    {
        for (auto& range : ranges)
        {
            const uint8_t* data = range.bytes.data();
            const size_t size = range.bytes.size();
            // Containers sit at unaligned offsets in the precache, so the scan steps by one byte.
            for (size_t i = 0; size > sizeof(ShaderContainer) && i < size - sizeof(ShaderContainer) - 1;)
            {
                auto container = reinterpret_cast<const ShaderContainer*>(data + i);
                const size_t dataSize = size_t(container->virtualSize) + container->physicalSize;
                if ((container->flags & 0xFFFFFF00) == 0x102A1100 && dataSize <= size - i &&
                    container->field1C == 0 && container->field20 == 0)
                {
                    if (validateContainer(data + i) == nullptr)
                        containers.try_emplace(XXH3_64bits(data + i, dataSize), data + i);
                    i += std::max<size_t>(dataSize, 1);
                }
                else
                {
                    i += 1;
                }
            }
        }
    }

    // Code the image stores outside any container is located by the hash of its bytes.
    const uint8_t* findCode(uint64_t hash, size_t size) const
    {
        for (auto& range : ranges)
        {
            for (size_t i = 0; size != 0 && i + size <= range.bytes.size(); i++)
            {
                if (XXH3_64bits(range.bytes.data() + i, size) == hash)
                    return range.bytes.data() + i;
            }
        }
        return nullptr;
    }
};

Corpus loadCorpus(int argc, char** argv, int first)
{
    Corpus corpus;
    for (int i = first; i < argc; i++)
        corpus.addFile(argv[i]);
    corpus.scan();
    fmt::println("[cc1] {} containers in {} scanned ranges", corpus.containers.size(), corpus.ranges.size());
    return corpus;
}

enum class RowSource
{
    Container, // source_hash names a shader container
    Code,      // source_hash names bare code bytes
    Inline     // the patches carry every word, for code the game compiles at run time
};

struct Row
{
    std::string hash;
    bool isPixelShader = false;
    bool translateWithContainer = false;
    RowSource source = RowSource::Container;
    uint64_t sourceHash = 0;
    uint32_t words = 0;
    std::vector<std::pair<uint32_t, uint32_t>> patches; // word index, big-endian word value
};

std::vector<std::string> splitLine(std::string_view line, char separator)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= line.size())
    {
        size_t stop = line.find(separator, start);
        if (stop == std::string_view::npos)
            stop = line.size();
        parts.emplace_back(line.substr(start, stop - start));
        start = stop + 1;
    }
    return parts;
}

std::vector<Row> readDeltas(const std::filesystem::path& path)
{
    const std::vector<uint8_t> data = readFile(path);
    std::vector<Row> rows;
    size_t lineNumber = 0;
    for (auto& rawLine : splitLine(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), '\n'))
    {
        std::string_view line = rawLine;
        lineNumber++;
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (lineNumber == 1 || line.empty())
            continue;

        const std::vector<std::string> cols = splitLine(line, ',');
        const auto bad = [&](const char* what)
            {
                return std::runtime_error(fmt::format("{} line {}: {}", path.string(), lineNumber, what));
            };
        if (cols.size() != 7)
            throw bad("expected 7 columns");
        Row row;
        row.hash = cols[0];
        if (row.hash.size() != 16)
            throw bad("runtime_ucode_hash is not 16 hex digits");
        if (cols[1] != "vs" && cols[1] != "ps")
            throw bad("type is neither vs nor ps");
        row.isPixelShader = cols[1] == "ps";
        if (cols[2] != "container" && cols[2] != "bare")
            throw bad("translate is neither container nor bare");
        row.translateWithContainer = cols[2] == "container";
        if (cols[3] == "container")
            row.source = RowSource::Container;
        else if (cols[3] == "code")
            row.source = RowSource::Code;
        else if (cols[3] == "inline")
            row.source = RowSource::Inline;
        else
            throw bad("source is not container, code or inline");
        if (row.translateWithContainer && row.source != RowSource::Container)
            throw bad("a container translation needs a container source");
        row.sourceHash = strtoull(cols[4].c_str(), nullptr, 16);
        row.words = uint32_t(strtoul(cols[5].c_str(), nullptr, 10));
        if (!cols[6].empty())
        {
            for (auto& patch : splitLine(cols[6], ';'))
            {
                const size_t colon = patch.find(':');
                if (colon == std::string::npos)
                    throw bad("a patch is not index:word");
                row.patches.emplace_back(uint32_t(strtoul(patch.substr(0, colon).c_str(), nullptr, 10)),
                    uint32_t(strtoul(patch.substr(colon + 1).c_str(), nullptr, 16)));
            }
        }
        rows.emplace_back(std::move(row));
    }
    return rows;
}

// Rebuilds the code the game hashes: the source bytes, or zeros for an inline row, with the row's words replaced.
const char* rebuildCode(const Row& row, const Corpus& corpus, std::vector<uint8_t>& code, const uint8_t*& container)
{
    CodeSpan span;
    container = nullptr;
    if (row.source == RowSource::Container)
    {
        auto found = corpus.containers.find(row.sourceHash);
        if (found == corpus.containers.end() || containerIsPixelShader(found->second) != row.isPixelShader)
            return "its source container is not in these game files";
        container = found->second;
        span = containerCode(container);
        if (span.size != size_t(row.words) * sizeof(uint32_t))
            return "its source container has another code size";
    }
    else if (row.source == RowSource::Code)
    {
        span = { corpus.findCode(row.sourceHash, size_t(row.words) * sizeof(uint32_t)), size_t(row.words) * sizeof(uint32_t) };
        if (span.data == nullptr)
            return "its source code is not in these game files";
    }
    else if (row.words == 0 || row.patches.size() != row.words)
    {
        return "its inline row does not list every word";
    }

    if (row.source == RowSource::Inline)
        code.assign(size_t(row.words) * sizeof(uint32_t), 0);
    else
        code.assign(span.data, span.data + span.size);
    for (auto& [index, value] : row.patches)
    {
        if (index >= row.words)
            return "a patch lies past the end of the code";
        writeBe32(code.data() + size_t(index) * sizeof(uint32_t), value);
    }
    if (XXH3_64bits(code.data(), code.size()) != strtoull(row.hash.c_str(), nullptr, 16))
        return "the rebuilt code does not hash to the runtime hash";
    return nullptr;
}

// CC1P: magic, version 1, file count, then per file a name length, the name and a size, then the bodies.
std::vector<uint8_t> packFiles(const std::map<std::string, std::vector<uint8_t>>& files)
{
    std::vector<uint8_t> pack;
    const auto put = [&](uint32_t value, size_t bytes)
        {
            for (size_t i = 0; i < bytes; i++)
                pack.push_back(uint8_t(value >> (8 * i)));
        };
    pack.insert(pack.end(), { 'C', 'C', '1', 'P' });
    put(1, 4);
    put(uint32_t(files.size()), 4);
    for (auto& [name, body] : files)
    {
        put(uint32_t(name.size()), 2);
        pack.insert(pack.end(), name.begin(), name.end());
        put(uint32_t(body.size()), 4);
    }
    for (auto& [name, body] : files)
        pack.insert(pack.end(), body.begin(), body.end());
    return pack;
}

struct Miss
{
    std::string hash;
    bool isPixelShader = false;
    std::vector<uint8_t> code; // guest byte order
};

// Miss dumps are named shader_<hash>.ucode.bin.<vert|frag> and hold host-order words.
bool readMiss(const std::filesystem::path& path, Miss& miss)
{
    const std::string name = path.filename().string();
    const bool isVertex = endsWithNoCase(name, ".ucode.bin.vert");
    if (name.rfind("shader_", 0) != 0 || name.size() != 7 + 16 + 15 || (!isVertex && !endsWithNoCase(name, ".ucode.bin.frag")))
        return false;
    miss.hash = name.substr(7, 16);
    for (auto& c : miss.hash)
        c = char(toupper(uint8_t(c)));
    miss.isPixelShader = !isVertex;
    miss.code = readFile(path);
    if (miss.code.empty() || (miss.code.size() & 3) != 0)
        return false;
    for (size_t i = 0; i < miss.code.size(); i += sizeof(uint32_t))
        writeBe32(miss.code.data() + i, readLe32(miss.code.data() + i));
    if (XXH3_64bits(miss.code.data(), miss.code.size()) != strtoull(miss.hash.c_str(), nullptr, 16))
    {
        fmt::println("[cc1] {} skipped: its contents do not hash to its name", name);
        return false;
    }
    return true;
}

// --cc1-pack <deltas csv> <rexglue shader common header> <out .pack> <game files...>
int runPack(int argc, char** argv)
{
    const std::filesystem::path deltasPath = argv[2];
    const std::vector<uint8_t> include = readFile(argv[3]);
    const std::filesystem::path outPath = argv[4];
    const Corpus corpus = loadCorpus(argc, argv, 5);
    const std::vector<Row> rows = readDeltas(deltasPath);

    std::vector<Cc1Shader> shaders;
    std::set<std::pair<std::string, bool>> seen;
    size_t unbuilt = 0, whole = 0;
    for (auto& row : rows)
    {
        if (!seen.insert({ row.hash, row.isPixelShader }).second)
            continue;
        Cc1Shader shader;
        const uint8_t* container = nullptr;
        if (const char* reason = rebuildCode(row, corpus, shader.code, container))
        {
            fmt::println("[cc1] {} {} left out: {}", row.hash, row.isPixelShader ? "ps" : "vs", reason);
            unbuilt++;
            continue;
        }
        shader.runtimeHash = row.hash;
        shader.isPixelShader = row.isPixelShader;
        shader.container = row.translateWithContainer ? container : nullptr;
        shaders.emplace_back(std::move(shader));
        whole += row.source == RowSource::Inline;
    }
    fmt::println("[cc1] {} of {} listed shaders rebuilt: {} from the game files, {} carried whole in the table",
        shaders.size(), seen.size(), shaders.size() - whole, whole);

    std::map<std::string, std::vector<uint8_t>> files;
    const int translated = buildCc1Shaders(shaders, std::string_view(reinterpret_cast<const char*>(include.data()), include.size()),
        [&](const std::string& name, const void* data, size_t size)
        {
            auto bytes = static_cast<const uint8_t*>(data);
            files[name].assign(bytes, bytes + size);
        });

    const std::vector<uint8_t> pack = packFiles(files);
    if (outPath.has_parent_path())
        std::filesystem::create_directories(outPath.parent_path());
    writeFile(outPath, pack.data(), pack.size());
    fmt::println("[cc1] pack: {} ({} files, {} bytes)", outPath.string(), files.size(), pack.size());
    return (unbuilt == 0 && translated == 0) ? 0 : 1;
}

// Lists the words that differ from the source, or every word when there is none.
std::string patchText(const uint8_t* source, const std::vector<uint8_t>& code)
{
    std::string patches;
    for (size_t i = 0; i < code.size(); i += sizeof(uint32_t))
    {
        if (source == nullptr || memcmp(source + i, code.data() + i, sizeof(uint32_t)) != 0)
            patches += fmt::format("{}{}:{:08X}", patches.empty() ? "" : ";", i / sizeof(uint32_t), readBe32(code.data() + i));
    }
    return patches;
}

// Bare code in the image that the runtime code derives from, bind-time vertex fetch patches aside.
const uint8_t* findCodeSource(const Corpus& corpus, const std::vector<uint8_t>& code, uint64_t fingerprint)
{
    const std::string_view whole(reinterpret_cast<const char*>(code.data()), code.size());
    for (auto& range : corpus.ranges)
    {
        const std::string_view bytes(reinterpret_cast<const char*>(range.bytes.data()), range.bytes.size());
        const size_t at = bytes.find(whole);
        if (at != std::string_view::npos)
            return range.bytes.data() + at;
    }
    // The first two words are control flow, which the bind-time patch never touches.
    const std::string_view prefix = whole.substr(0, std::min<size_t>(whole.size(), 8));
    for (auto& range : corpus.ranges)
    {
        const std::string_view bytes(reinterpret_cast<const char*>(range.bytes.data()), range.bytes.size());
        for (size_t at = bytes.find(prefix); at != std::string_view::npos; at = bytes.find(prefix, at + 1))
        {
            if (at + code.size() <= bytes.size() && fingerprintOf(range.bytes.data() + at, code.size()) == fingerprint)
                return range.bytes.data() + at;
        }
    }
    return nullptr;
}

// --cc1-add-misses <deltas csv> <misses dir> <game files...>
int runAddMisses(int argc, char** argv)
{
    const std::filesystem::path deltasPath = argv[2];
    const std::filesystem::path missesPath = argv[3];
    const Corpus corpus = loadCorpus(argc, argv, 4);

    std::set<std::pair<std::string, bool>> known;
    std::string text;
    if (std::filesystem::exists(deltasPath))
    {
        for (auto& row : readDeltas(deltasPath))
            known.insert({ row.hash, row.isPixelShader });
        const std::vector<uint8_t> data = readFile(deltasPath);
        text.assign(data.begin(), data.end());
    }
    if (text.empty())
        text = std::string(kDeltasHeader) + "\n";
    if (text.back() != '\n')
        text += '\n';

    // Containers by stage, vertex-fetch-insensitive fingerprint and code size, in hash order.
    std::map<std::tuple<bool, uint64_t, size_t>, std::vector<uint64_t>> byFingerprint;
    for (auto& [hash, base] : corpus.containers)
    {
        const CodeSpan span = containerCode(base);
        byFingerprint[{ containerIsPixelShader(base), fingerprintOf(span.data, span.size), span.size }].push_back(hash);
    }

    std::vector<std::filesystem::path> paths;
    for (auto& file : std::filesystem::directory_iterator(missesPath))
    {
        if (file.is_regular_file())
            paths.push_back(file.path());
    }
    std::sort(paths.begin(), paths.end());

    size_t fromContainer = 0, fromCode = 0, whole = 0;
    for (auto& path : paths)
    {
        Miss miss;
        if (!readMiss(path, miss) || !known.insert({ miss.hash, miss.isPixelShader }).second)
            continue;

        const uint64_t fingerprint = fingerprintOf(miss.code.data(), miss.code.size());
        const char* type = miss.isPixelShader ? "ps" : "vs";
        const size_t words = miss.code.size() / sizeof(uint32_t);
        auto candidates = byFingerprint.find({ miss.isPixelShader, fingerprint, miss.code.size() });
        if (candidates != byFingerprint.end())
        {
            // An identical container wins over the first one in hash order.
            uint64_t source = candidates->second.front();
            for (uint64_t hash : candidates->second)
            {
                if (memcmp(containerCode(corpus.containers.at(hash)).data, miss.code.data(), miss.code.size()) == 0)
                {
                    source = hash;
                    break;
                }
            }
            text += fmt::format("{},{},bare,container,{:016X},{},{}\n", miss.hash, type, source, words,
                patchText(containerCode(corpus.containers.at(source)).data, miss.code));
            fromContainer++;
        }
        else if (const uint8_t* source = findCodeSource(corpus, miss.code, fingerprint))
        {
            text += fmt::format("{},{},bare,code,{:016X},{},{}\n", miss.hash, type,
                XXH3_64bits(source, miss.code.size()), words, patchText(source, miss.code));
            fromCode++;
        }
        else
        {
            size_t nearest = SIZE_MAX;
            for (auto& [hash, base] : corpus.containers)
            {
                const CodeSpan span = containerCode(base);
                if (containerIsPixelShader(base) != miss.isPixelShader || span.size != miss.code.size())
                    continue;
                size_t differing = 0;
                for (size_t i = 0; i < span.size; i += sizeof(uint32_t))
                    differing += memcmp(span.data + i, miss.code.data() + i, sizeof(uint32_t)) != 0;
                nearest = std::min<size_t>(nearest, differing);
            }
            if (nearest == SIZE_MAX)
                fmt::println("[cc1] {} {}: added whole, the game files hold no container of {} words", miss.hash, type, words);
            else
                fmt::println("[cc1] {} {}: added whole, the nearest container differs in {} of {} words",
                    miss.hash, type, nearest, words);
            // Code the game compiles at run time has no source in its files, so the row carries every word.
            text += fmt::format("{},{},bare,inline,,{},{}\n", miss.hash, type, words, patchText(nullptr, miss.code));
            whole++;
        }
    }

    const size_t added = fromContainer + fromCode + whole;
    if (added != 0)
    {
        if (deltasPath.has_parent_path())
            std::filesystem::create_directories(deltasPath.parent_path());
        writeFile(deltasPath, text.data(), text.size());
    }
    fmt::println("[cc1] {} rows added to {}: {} from containers, {} from image code, {} whole", added,
        deltasPath.string(), fromContainer, fromCode, whole);
    return 0;
}

// --cc1-scan <game files...>
int runScan(int argc, char** argv)
{
    const Corpus corpus = loadCorpus(argc, argv, 2);
    for (auto& range : corpus.ranges)
    {
        size_t vertex = 0, pixel = 0;
        for (auto& [hash, base] : corpus.containers)
        {
            if (base < range.bytes.data() || base >= range.bytes.data() + range.bytes.size())
                continue;
            (containerIsPixelShader(base) ? pixel : vertex)++;
        }
        fmt::println("{}: {} bytes, {:016X}, {} vs + {} ps containers first seen here", range.name, range.bytes.size(),
            uint64_t(XXH3_64bits(range.bytes.data(), range.bytes.size())), vertex, pixel);
    }
    return 0;
}

}

int runCc1(int argc, char** argv)
{
    if (argc >= 6 && strcmp(argv[1], "--cc1-pack") == 0)
        return runPack(argc, argv);
    if (argc >= 5 && strcmp(argv[1], "--cc1-add-misses") == 0)
        return runAddMisses(argc, argv);
    if (argc >= 3 && strcmp(argv[1], "--cc1-scan") == 0)
        return runScan(argc, argv);
    fprintf(stderr, "usage: --cc1-pack <deltas csv> <rexglue shader common header> <out .pack> <game files...>\n"
                    "       --cc1-add-misses <deltas csv> <misses dir> <game files...>\n"
                    "       --cc1-scan <game files...>\n");
    return 1;
}
