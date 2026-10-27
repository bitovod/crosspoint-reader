#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "PdbReader.h"

namespace
{

using Bytes = std::vector<uint8_t>;

void putBe16(Bytes& b, size_t pos, uint16_t v)
{
    b[pos] = static_cast<uint8_t>(v >> 8);
    b[pos + 1] = static_cast<uint8_t>(v);
}

void putBe32(Bytes& b, size_t pos, uint32_t v)
{
    b[pos] = static_cast<uint8_t>(v >> 24);
    b[pos + 1] = static_cast<uint8_t>(v >> 16);
    b[pos + 2] = static_cast<uint8_t>(v >> 8);
    b[pos + 3] = static_cast<uint8_t>(v);
}

// Builds a PDB file with a PalmDOC record 0 followed by the given records.
Bytes buildPdb(uint16_t compression, uint32_t textLength, uint16_t textRecords, const std::vector<Bytes>& records,
               const char* type = "TEXtREAd")
{
    const uint16_t numRecords = static_cast<uint16_t>(records.size() + 1);
    Bytes file(78 + 8 * numRecords + 2, 0);
    std::memcpy(file.data(), "Test book", 9);
    std::memcpy(file.data() + 60, type, 8);
    putBe16(file, 76, numRecords);

    Bytes rec0(16, 0);
    putBe16(rec0, 0, compression);
    putBe32(rec0, 4, textLength);
    putBe16(rec0, 8, textRecords);
    putBe16(rec0, 10, 4096);

    std::vector<Bytes> all;
    all.push_back(rec0);
    all.insert(all.end(), records.begin(), records.end());

    for (size_t i = 0; i < all.size(); i++)
    {
        putBe32(file, 78 + 8 * i, static_cast<uint32_t>(file.size()));
        file.insert(file.end(), all[i].begin(), all[i].end());
    }
    return file;
}

int readAt(void* ctx, uint32_t offset, uint8_t* buf, size_t size)
{
    const auto* file = static_cast<const Bytes*>(ctx);
    if (offset > file->size())
    {
        return -1;
    }
    const size_t n = std::min(size, file->size() - offset);
    std::memcpy(buf, file->data() + offset, n);
    return static_cast<int>(n);
}

std::string readAll(Bytes& file, size_t chunk, PdbReader::Status* status = nullptr)
{
    PdbReader reader;
    const PdbReader::Status st = reader.open(&file, readAt, static_cast<uint32_t>(file.size()));
    if (status)
    {
        *status = st;
    }
    if (st != PdbReader::Status::Ok)
    {
        return "";
    }
    std::string out;
    std::vector<uint8_t> buf(chunk);
    int n = 0;
    while ((n = reader.read(buf.data(), buf.size())) > 0)
    {
        out.append(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(n));
    }
    EXPECT_EQ(n, 0);
    return out;
}

// "Hello" literals, 0xF7 = space + 'w', "orld\n", back reference to "Hello"
// (distance 12, length 5), 2 raw bytes (z/s caron), 1 raw byte (S caron).
const Bytes kRecord1 = {'H', 'e', 'l', 'l', 'o', 0xF7, 'o', 'r', 'l', 'd', '\n',
                        0x80, 0x62, 0x02, 0x9E, 0x9A, 0x01, 0x8A};
// '!' and the undefined Windows-1250 byte 0x81.
const Bytes kRecord2 = {'!', 0x01, 0x81};
const std::string kExpected = "Hello world\nHello\xC5\xBE\xC5\xA1\xC5\xA0!?";

TEST(PdbReader, DecompressesAndConvertsToUtf8)
{
    const Bytes bookmark(20, 'B');
    Bytes file = buildPdb(2, 21, 2, {kRecord1, kRecord2, bookmark});
    EXPECT_EQ(readAll(file, 4096), kExpected);
}

TEST(PdbReader, HandlesOneByteReads)
{
    Bytes file = buildPdb(2, 21, 2, {kRecord1, kRecord2});
    EXPECT_EQ(readAll(file, 1), kExpected);
}

TEST(PdbReader, ReadsUncompressedRecords)
{
    const Bytes rec = {'A', ' ', 0xE8, 'e', 'r', 'v', 'e', 'n', 0xFD};
    Bytes file = buildPdb(1, 9, 1, {rec});
    EXPECT_EQ(readAll(file, 64), "A \xC4\x8D" "erven\xC3\xBD");
}

TEST(PdbReader, ClampsRecordCountToFile)
{
    Bytes file = buildPdb(2, 21, 50, {kRecord1, kRecord2});
    EXPECT_EQ(readAll(file, 64), kExpected);
}

TEST(PdbReader, SkipsInvalidBackReference)
{
    // Distance 100 points before the start of the record.
    const Bytes rec = {'a', 0x83, 0x20, 'b'};
    Bytes file = buildPdb(2, 2, 1, {rec});
    EXPECT_EQ(readAll(file, 64), "ab");
}

TEST(PdbReader, RejectsOtherFormats)
{
    PdbReader::Status status = PdbReader::Status::Ok;

    Bytes mobi = buildPdb(2, 21, 1, {kRecord1}, "BOOKMOBI");
    readAll(mobi, 64, &status);
    EXPECT_EQ(status, PdbReader::Status::NotPalmDoc);

    Bytes huff = buildPdb(17480, 21, 1, {kRecord1});
    readAll(huff, 64, &status);
    EXPECT_EQ(status, PdbReader::Status::UnsupportedCompression);

    Bytes tiny(40, 0);
    readAll(tiny, 64, &status);
    EXPECT_EQ(status, PdbReader::Status::BadHeader);
}

}  // namespace
