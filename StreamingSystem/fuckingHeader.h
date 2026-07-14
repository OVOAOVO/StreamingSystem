#pragma once

#include <iostream>
#include <atomic>
#include <cstdint>
#include <cassert>


using byte = uint8_t;


// ==========================
// Constants
// ==========================

#define STREAM_BUFFER_SIZE      (256 * 1024)
#define STREAM_BASE_LATENCY_MS  (1000)
#define SOURCE_BUFFER_COUNT     2

#define assertint(condition, value)  assert(condition)


// ==========================
// StreamEntry
// ==========================

struct StreamEntry
{
    int64_t  size         = 0;
    int64_t  offset       = 0;
    int      channelCount = 1;
    bool     looping      = false;
};


// ==========================
// Forward declarations (external API)
// ==========================

struct StreamBuffer;

StreamBuffer* StreamBufferRead(const char* filename, unsigned latencyMs,
                               int64_t offset, int64_t size, void* fileHandle);
bool StreamBufferReady(const StreamBuffer* buffer);
bool DecoderBufferReady(const class Decoder* decoder);
void DecoderBufferSubmit(Decoder* decoder, const byte* data,
                         int64_t size, bool isLast);


// ==========================
// StringHash
// ==========================

class StringHash
{
public:

    uint32_t value = 0;

    StringHash() = default;


    explicit StringHash(const char* str)
    {
        value = HashString(str);
    }


    static uint32_t HashString(const char* str)
    {
        uint32_t hash = 2166136261u;

        while (*str)
        {
            hash ^= static_cast<uint8_t>(*str++);
            hash *= 16777619u;
        }

        return hash;
    }


    bool operator==(const StringHash& other) const
    {
        return value == other.value;
    }
};



// ==========================
// StreamId
// ==========================

using StreamId = uint64_t;



// ==========================
// StreamBuffer
// ==========================

struct StreamBuffer
{
    std::atomic<int32_t> refCount{ 0 };

    const char* filename = nullptr;

    StringHash filenameHash;

    int64_t offset = 0;

    int64_t readSize = 0;


    uint32_t requestLatency = 0;

    uint32_t requestStartTime = 0;

    uint32_t requestEndTime = 0;


    StreamId requestId = 0;


    byte* data = nullptr;


    bool valid = false;

    bool error = false;

    bool invalidateBuffer = false;


    bool primed = false;
};



// ==========================
// Forward declaration
// ==========================

struct Stream;



// ==========================
// Source
// ==========================

class Source
{
public:

    Stream*      stream = nullptr;
    StreamEntry* entry  = nullptr;


    bool error  = false;
    bool eos    = false;
    bool primed = false;
};



// ==========================
// Decoder
// ==========================

class Decoder
{
public:

    bool done = false;

    bool error = false;

    bool eos = false;
};



// ==========================
// Stream
// ==========================

class Stream
{
public:

    const char*   filename   = nullptr;
    void*         fileHandle = nullptr;

    StreamEntry*  entry      = nullptr;

    StreamBuffer* ioBuffer   = nullptr;


    static constexpr int BUFFER_COUNT = 2;


    StreamBuffer* buffers[BUFFER_COUNT]
    {
        nullptr,
        nullptr
    };


    bool buffersSubmitted[BUFFER_COUNT]
    {
        false,
        false
    };


    int64_t readOffset  = 0;
    StreamBuffer* lastBuffer = nullptr;


    // ---- query helpers (used by SourceStreamUpdateStep) ----

    /// True when no IO is in flight, or triple-buffer needs topping up.
    bool NeedsIORequest() const
    {
        if (ioBuffer == nullptr)
            return true;

        if (buffers[1] && buffers[1]->readSize < STREAM_BUFFER_SIZE)
            return true;

        return false;
    }

    /// Bytes still to be read from this entry.
    int64_t GetRemainingBytes() const
    {
        const int64_t remaining = entry->size - readOffset;
        assertint(remaining >= 0, entry->size);
        return remaining;
    }

    /// Clamp the next read chunk to the stream-buffer cap.
    int64_t CalculateReadSize() const
    {
        int64_t size = GetRemainingBytes();
        if (size > STREAM_BUFFER_SIZE)
            size = STREAM_BUFFER_SIZE;
        return size;
    }

    /// Lower latency for non-zero-offset reads (per-channel scaling).
    unsigned CalculateReadLatency() const
    {
        if (readOffset == 0)
            return 0;
        return STREAM_BASE_LATENCY_MS / entry->channelCount;
    }
};