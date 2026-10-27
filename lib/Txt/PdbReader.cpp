#include "PdbReader.h"

#include <cstring>
#include <new>

namespace
{

// Windows-1250 code points for bytes 0x80..0xFF. Undefined bytes map to '?'.
constexpr uint16_t CP1250_HIGH[128] = {
    0x20AC, 0x003F, 0x201A, 0x003F, 0x201E, 0x2026, 0x2020, 0x2021,
    0x003F, 0x2030, 0x0160, 0x2039, 0x015A, 0x0164, 0x017D, 0x0179,
    0x003F, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x003F, 0x2122, 0x0161, 0x203A, 0x015B, 0x0165, 0x017E, 0x017A,
    0x00A0, 0x02C7, 0x02D8, 0x0141, 0x00A4, 0x0104, 0x00A6, 0x00A7,
    0x00A8, 0x00A9, 0x015E, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x017B,
    0x00B0, 0x00B1, 0x02DB, 0x0142, 0x00B4, 0x00B5, 0x00B6, 0x00B7,
    0x00B8, 0x0105, 0x015F, 0x00BB, 0x013D, 0x02DD, 0x013E, 0x017C,
    0x0154, 0x00C1, 0x00C2, 0x0102, 0x00C4, 0x0139, 0x0106, 0x00C7,
    0x010C, 0x00C9, 0x0118, 0x00CB, 0x011A, 0x00CD, 0x00CE, 0x010E,
    0x0110, 0x0143, 0x0147, 0x00D3, 0x00D4, 0x0150, 0x00D6, 0x00D7,
    0x0158, 0x016E, 0x00DA, 0x0170, 0x00DC, 0x00DD, 0x0162, 0x00DF,
    0x0155, 0x00E1, 0x00E2, 0x0103, 0x00E4, 0x013A, 0x0107, 0x00E7,
    0x010D, 0x00E9, 0x0119, 0x00EB, 0x011B, 0x00ED, 0x00EE, 0x010F,
    0x0111, 0x0144, 0x0148, 0x00F3, 0x00F4, 0x0151, 0x00F6, 0x00F7,
    0x0159, 0x016F, 0x00FA, 0x0171, 0x00FC, 0x00FD, 0x0163, 0x02D9,
};

constexpr uint16_t COMPRESSION_NONE = 1;
constexpr uint16_t COMPRESSION_PALMDOC = 2;

uint16_t readBe16(const uint8_t* p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t readBe32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

}  // namespace

bool PdbReader::isPalmDocHeader(const uint8_t* header, const size_t size)
{
    // Database type "TEXt"; the creator differs between tools (REAd, TlDc, ...).
    return size >= 68 && std::memcmp(header + 60, "TEXt", 4) == 0;
}

PdbReader::Status PdbReader::open(void* ctx, const ReadAtFn readAt, const uint32_t fileSize)
{
    readCtx = ctx;
    readAtFn = readAt;
    fileLen = fileSize;
    failed = false;

    if (fileLen < PDB_HEADER_SIZE + RECORD_ENTRY_SIZE)
    {
        return Status::BadHeader;
    }

    uint8_t header[PDB_HEADER_SIZE];
    if (readAtFn(readCtx, 0, header, sizeof(header)) != static_cast<int>(sizeof(header)))
    {
        return Status::ReadError;
    }
    if (!isPalmDocHeader(header, sizeof(header)))
    {
        return Status::NotPalmDoc;
    }

    numRecords = readBe16(header + 76);
    if (numRecords < 1 || PDB_HEADER_SIZE + static_cast<uint32_t>(numRecords) * RECORD_ENTRY_SIZE > fileLen)
    {
        return Status::BadHeader;
    }

    uint32_t rec0Start = 0;
    uint32_t rec0Len = 0;
    if (!recordBounds(0, rec0Start, rec0Len) || rec0Len < 12)
    {
        return Status::BadHeader;
    }

    uint8_t rec0[12];
    if (readAtFn(readCtx, rec0Start, rec0, sizeof(rec0)) != static_cast<int>(sizeof(rec0)))
    {
        return Status::ReadError;
    }

    compression = readBe16(rec0);
    textLen = readBe32(rec0 + 4);
    recordCount = readBe16(rec0 + 8);
    uint16_t recordSize = readBe16(rec0 + 10);

    if (compression != COMPRESSION_NONE && compression != COMPRESSION_PALMDOC)
    {
        return Status::UnsupportedCompression;
    }

    // Records after the text records hold bookmarks; they are ignored.
    if (recordCount > numRecords - 1)
    {
        recordCount = static_cast<uint16_t>(numRecords - 1);
    }

    if (recordSize < 4096)
    {
        recordSize = 4096;
    }
    if (recordSize > MAX_RECORD_SIZE)
    {
        recordSize = MAX_RECORD_SIZE;
    }
    recordCapacity = recordSize;

    // One record buffer for the whole read; freed with the reader.
    recBuf.reset(new (std::nothrow) uint8_t[recordCapacity]);
    if (!recBuf)
    {
        return Status::OutOfMemory;
    }

    recLen = 0;
    recPos = 0;
    nextRecord = 1;
    pendingLen = 0;
    pendingPos = 0;
    return Status::Ok;
}

bool PdbReader::recordBounds(const uint16_t index, uint32_t& start, uint32_t& length) const
{
    uint8_t entry[RECORD_ENTRY_SIZE + 4];
    const uint32_t entryOffset = PDB_HEADER_SIZE + static_cast<uint32_t>(index) * RECORD_ENTRY_SIZE;
    const bool hasNext = index + 1 < numRecords;
    const size_t want = hasNext ? sizeof(entry) : 4;

    if (readAtFn(readCtx, entryOffset, entry, want) != static_cast<int>(want))
    {
        return false;
    }

    start = readBe32(entry);
    const uint32_t end = hasNext ? readBe32(entry + RECORD_ENTRY_SIZE) : fileLen;
    if (start > end || end > fileLen)
    {
        return false;
    }
    length = end - start;
    return true;
}

bool PdbReader::nextInByte(uint8_t& b)
{
    if (inChunkPos >= inChunkLen)
    {
        if (inRemaining == 0)
        {
            return false;
        }
        const size_t want = inRemaining < IN_CHUNK_SIZE ? inRemaining : IN_CHUNK_SIZE;
        const int got = readAtFn(readCtx, inFileOffset, inChunk, want);
        if (got <= 0)
        {
            failed = true;
            return false;
        }
        inChunkLen = static_cast<size_t>(got);
        inChunkPos = 0;
        inFileOffset += static_cast<uint32_t>(got);
        inRemaining -= static_cast<uint32_t>(got);
    }
    b = inChunk[inChunkPos++];
    return true;
}

bool PdbReader::decompressRecord()
{
    uint8_t* out = recBuf.get();
    size_t outPos = 0;
    uint8_t c = 0;

    while (outPos < recordCapacity && nextInByte(c))
    {
        if (c == 0x00 || (c >= 0x09 && c <= 0x7F))
        {
            // Literal byte
            out[outPos++] = c;
        }
        else if (c <= 0x08)
        {
            // Copy the next c bytes verbatim
            for (uint8_t i = 0; i < c && outPos < recordCapacity; i++)
            {
                uint8_t lit = 0;
                if (!nextInByte(lit))
                {
                    break;
                }
                out[outPos++] = lit;
            }
        }
        else if (c >= 0xC0)
        {
            // Space followed by an ASCII character
            out[outPos++] = ' ';
            if (outPos < recordCapacity)
            {
                out[outPos++] = static_cast<uint8_t>(c ^ 0x80);
            }
        }
        else
        {
            // 0x80..0xBF: LZ77 back reference, 11-bit distance and 3-bit length
            uint8_t c2 = 0;
            if (!nextInByte(c2))
            {
                break;
            }
            const uint16_t pair = static_cast<uint16_t>(((c << 8) | c2) & 0x3FFF);
            const size_t distance = pair >> 3;
            const size_t length = (pair & 0x07) + 3;
            if (distance == 0 || distance > outPos)
            {
                // Corrupt reference; skip it instead of reading outside the buffer.
                continue;
            }
            for (size_t i = 0; i < length && outPos < recordCapacity; i++)
            {
                out[outPos] = out[outPos - distance];
                outPos++;
            }
        }
    }

    recLen = outPos;
    return !failed;
}

bool PdbReader::loadRecord(const uint16_t index)
{
    uint32_t start = 0;
    uint32_t length = 0;
    if (!recordBounds(index, start, length))
    {
        return false;
    }

    inFileOffset = start;
    inRemaining = length;
    inChunkLen = 0;
    inChunkPos = 0;
    recPos = 0;
    recLen = 0;

    if (compression == COMPRESSION_PALMDOC)
    {
        return decompressRecord();
    }

    // Uncompressed record: copy as is, truncated to the buffer size.
    uint8_t* out = recBuf.get();
    uint8_t c = 0;
    while (recLen < recordCapacity && nextInByte(c))
    {
        out[recLen++] = c;
    }
    return !failed;
}

int PdbReader::read(uint8_t* buf, const size_t size)
{
    if (failed || !recBuf)
    {
        return -1;
    }

    size_t written = 0;
    while (written < size)
    {
        if (pendingPos < pendingLen)
        {
            buf[written++] = pending[pendingPos++];
            continue;
        }

        if (recPos >= recLen)
        {
            if (nextRecord > recordCount)
            {
                break;
            }
            if (!loadRecord(nextRecord++))
            {
                failed = true;
                return -1;
            }
            continue;
        }

        const uint8_t b = recBuf[recPos++];
        if (b < 0x80)
        {
            buf[written++] = b;
            continue;
        }

        // All Windows-1250 characters are in the BMP: 2 or 3 UTF-8 bytes.
        const uint16_t cp = CP1250_HIGH[b - 0x80];
        if (cp < 0x80)
        {
            // Undefined byte mapped to '?'
            buf[written++] = static_cast<uint8_t>(cp);
            continue;
        }
        if (cp < 0x800)
        {
            pending[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
            pending[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
            pendingLen = 2;
        }
        else
        {
            pending[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
            pending[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
            pending[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
            pendingLen = 3;
        }
        pendingPos = 0;
    }

    return static_cast<int>(written);
}

int PdbReader::readCallback(void* ctx, uint8_t* buf, const size_t size)
{
    return static_cast<PdbReader*>(ctx)->read(buf, size);
}
