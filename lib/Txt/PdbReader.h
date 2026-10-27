#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

// Streaming reader for PalmDOC e-books (.pdb, type "TEXt", e.g. "TEXtREAd").
// Decompresses one 4 KB text record at a time and converts Windows-1250 text
// to UTF-8, so the output can be fed directly into TxtToHtml::stream().
// The reader is storage-agnostic: the caller supplies a random-access read
// callback, which keeps the class usable in host tests.
class PdbReader {
 public:
  // Reads up to size bytes at the given absolute file offset.
  // Returns the number of bytes read, or a negative value on error.
  using ReadAtFn = int (*)(void* ctx, uint32_t offset, uint8_t* buf, size_t size);

  enum class Status : uint8_t {
    Ok,
    NotPalmDoc,
    BadHeader,
    UnsupportedCompression,
    OutOfMemory,
    ReadError,
  };

  PdbReader() = default;
  PdbReader(const PdbReader&) = delete;
  PdbReader& operator=(const PdbReader&) = delete;

  // Parses the PDB header and PalmDOC record 0. Allocates the record buffer.
  Status open(void* ctx, ReadAtFn readAt, uint32_t fileSize);

  // Fills buf with UTF-8 text. Returns bytes written, 0 at end, -1 on error.
  int read(uint8_t* buf, size_t size);

  // Adapter matching the TxtToHtml::stream() read callback (ctx = PdbReader*).
  static int readCallback(void* ctx, uint8_t* buf, size_t size);

  // Quick check of the type field at offset 60 (expects at least 68 bytes).
  static bool isPalmDocHeader(const uint8_t* header, size_t size);

  uint32_t textLength() const { return textLen; }

  uint16_t textRecordCount() const { return recordCount; }

 private:
  static constexpr uint32_t PDB_HEADER_SIZE = 78;
  static constexpr uint32_t RECORD_ENTRY_SIZE = 8;
  static constexpr size_t IN_CHUNK_SIZE = 256;
  // PalmDOC records are nominally 4096 bytes; allow some slack for odd encoders.
  static constexpr uint16_t MAX_RECORD_SIZE = 8192;

  bool recordBounds(uint16_t index, uint32_t& start, uint32_t& length) const;
  bool loadRecord(uint16_t index);
  bool nextInByte(uint8_t& b);
  bool decompressRecord();

  void* readCtx = nullptr;
  ReadAtFn readAtFn = nullptr;
  uint32_t fileLen = 0;

  uint16_t numRecords = 0;
  uint16_t compression = 0;
  uint16_t recordCount = 0;
  uint16_t recordCapacity = 0;
  uint32_t textLen = 0;

  // Decompressed text of the current record (Windows-1250).
  std::unique_ptr<uint8_t[]> recBuf;
  size_t recLen = 0;
  size_t recPos = 0;
  uint16_t nextRecord = 1;

  // Buffered input from the compressed record.
  uint8_t inChunk[IN_CHUNK_SIZE] = {};
  uint32_t inFileOffset = 0;
  uint32_t inRemaining = 0;
  size_t inChunkLen = 0;
  size_t inChunkPos = 0;

  // UTF-8 bytes of a converted character that did not fit into the caller buffer.
  uint8_t pending[3] = {};
  uint8_t pendingLen = 0;
  uint8_t pendingPos = 0;

  bool failed = false;
};
