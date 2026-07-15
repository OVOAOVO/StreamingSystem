#include "fuckingHeader.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

using namespace std;

// bridge the C-style name used by the external API
typedef StreamBuffer stream_buffer;


// ====================================================================
// Mock / stub implementations of the external platform API.
// In a real engine these would talk to the OS I/O system and a
// hardware audio decoder.  Here they simulate async behaviour so the
// whole pipeline compiles and runs as a self-contained demo.
// ====================================================================

// --- mock globals ---------------------------------------------------
static unsigned g_mockTimeMs = 0;          // fake timer for async IO

// --- StreamBufferRead ------------------------------------------------
// Pretends to issue an async read.  Allocates a dummy buffer and marks
// it "ready" after a simulated latency.
StreamBuffer* StreamBufferRead(const char*   filename,
                               unsigned      latencyMs,
                               int64_t       offset,
                               int64_t       size,
                               void*         fileHandle)
{
    (void)filename;
    (void)fileHandle;

    // allocate a new buffer for the async request
    StreamBuffer* buf        = new StreamBuffer();
    buf->data                = new byte[size];
    buf->readSize            = size;
    buf->offset              = offset;
    buf->filename            = filename;
    buf->requestLatency      = latencyMs;
    buf->requestStartTime    = g_mockTimeMs;
    buf->requestEndTime      = g_mockTimeMs + latencyMs;
    buf->valid               = false;   // not ready yet
    buf->error               = false;
    buf->refCount.store(1);

    // fill with pretend audio data (sawtooth for audibility if dumped)
    for (int64_t i = 0; i < size; i++)
        buf->data[i] = (byte)((offset + i) & 0xFF);

    printf("  [IO]    StreamBufferRead  offset=%8lld  size=%8lld  latency=%u ms\n",
           (long long)offset, (long long)size, latencyMs);

    return buf;
}

// --- StreamBufferReady -----------------------------------------------
// Returns true when the async IO has completed (mock: time-based).
bool StreamBufferReady(const StreamBuffer* buffer)
{
    if (!buffer) return false;
    if (buffer->error) return true;   // errors are "done" too

    // simulate async completion after the latency window
    bool ready = (g_mockTimeMs >= buffer->requestEndTime);
    if (ready)
    {
        // const_cast so we can mark it ready (real impl would be atomic)
        const_cast<StreamBuffer*>(buffer)->valid = true;
    }
    return ready || buffer->valid;
}

// --- DecoderBufferReady ----------------------------------------------
// Returns true when the decoder can accept more compressed data.
bool DecoderBufferReady(const Decoder* decoder)
{
    if (!decoder) return false;
    // decoder can take data unless it's done, errored, or at EOS
    return !decoder->done && !decoder->error && !decoder->eos;
}

// --- DecoderBufferSubmit ---------------------------------------------
// Feeds a buffer of compressed data to the decoder.
void DecoderBufferSubmit(Decoder*     decoder,
                         const byte*  data,
                         int64_t      size,
                         bool         isLast)
{
    (void)data;
    (void)size;

    printf("  [DEC]   DecoderBufferSubmit  size=%8lld  last=%s\n",
           (long long)size, isLast ? "true" : "false");

    if (isLast)
        decoder->eos = true;
}


// ====================================================================
// StreamBufferReadPrimed
//   Initialises a pre-allocated buffer with already-in-memory data
//   (used for tiny files that fit in a single buffer or priming).
// ====================================================================

stream_buffer* StreamBufferReadPrimed(stream_buffer* buffer,
                                      int64_t        offset,
                                      const byte*    primeData,
                                      unsigned       size)
{
    assert(size);
    assert(size <= STREAM_BUFFER_SIZE);
    assert(buffer);
    assert(buffer->refCount == 0);

    buffer->offset       = offset;
    buffer->valid        = true;
    buffer->error        = false;
    buffer->primed       = true;
    buffer->filename     = nullptr;
    buffer->filenameHash = StringHash();
    buffer->data         = const_cast<byte*>(primeData);
    buffer->readSize     = size;
    buffer->refCount.store(buffer->refCount.load() + 1);

    return buffer;
}


// ====================================================================
// ValidateStream
//   Debug-only sanity checks on stream invariants.
// ====================================================================

void ValidateStream(const Stream* stream)
{
    assert(stream);

    assert(!stream->ioBuffer ||
           stream->ioBuffer->refCount > 0);

    assert(!stream->buffers[0] ||
           stream->buffers[0]->refCount > 0);

    assert(!stream->buffers[1] ||
           stream->buffers[1]->refCount > 0);

    assert(stream->filename != nullptr);
}


// ====================================================================
// IsIOBufferReady
//   Has the in-flight IO request completed?
// ====================================================================

bool IsIOBufferReady(const Stream* stream)
{
    if (stream->ioBuffer == nullptr)
        return false;

    return StreamBufferReady(stream->ioBuffer);
}


// ====================================================================
// HandleSuccessfulRead
//   Called after StreamBufferRead returns a valid ioBuffer.
//   Advances readOffset; decides whether to loop or signal EOS.
// ====================================================================

static void HandleSuccessfulRead(Stream* stream, Source* source, int64_t size)
{
    stream->readOffset += size;
    assertint(stream->entry->size - stream->readOffset >= 0,
              stream->entry->size);

    if (stream->readOffset == stream->entry->size)
    {
        // single-buffer looping files don't issue further IO
        if (source->entry->looping &&
            stream->entry->size != stream->ioBuffer->readSize)
        {
            stream->readOffset = 0;
            printf("  [LOOP]  readOffset reset to 0\n");
        }
        else
        {
            source->eos        = true;
            stream->lastBuffer = stream->ioBuffer;
            printf("  [EOS]   end of stream reached\n");
        }
    }

    assert(stream->readOffset <= stream->entry->size);
}


// ====================================================================
// CanSubmitBuffer
//   Returns true when buffer[index] can be handed to the decoder.
// ====================================================================

static bool CanSubmitBuffer(const Stream*  stream,
                            const Decoder* decoder,
                            int            index)
{
    if (stream->buffersSubmitted[index])
        return false;

    if (stream->buffers[index] == nullptr)
        return false;

    if (!DecoderBufferReady(decoder))
        return false;

    if (decoder->error)
        return false;

    return true;
}


// ====================================================================
// SubmitSingleBuffer
//   Hands one buffer to the decoder and marks it submitted.
// ====================================================================

static void SubmitSingleBuffer(Stream* stream, Decoder* decoder, int index)
{
    assert(!decoder->eos);
    assert(stream->buffers[index]->refCount);

    DecoderBufferSubmit(decoder,
                        stream->buffers[index]->data,
                        stream->buffers[index]->readSize,
                        stream->buffers[index] == stream->lastBuffer);

    stream->buffersSubmitted[index] = true;

    assert(decoder->eos == (stream->buffers[index] == stream->lastBuffer));
}


// ====================================================================
// SubmitBuffersToDecoder
//   Iterates all buffers; submits each one that is ready.
// ====================================================================

static void SubmitBuffersToDecoder(Stream*  stream,
                                   Source*  source,
                                   Decoder* decoder)
{
    if (!source->primed)
        return;

    for (int i = 0; i < SOURCE_BUFFER_COUNT; i++)
    {
        if (CanSubmitBuffer(stream, decoder, i))
        {
            SubmitSingleBuffer(stream, decoder, i);
        }
    }
}


// ====================================================================
// SourceStreamUpdateStep
//   Main update tick for one streaming source.
//   Returns true when new data was consumed this step.
//   This is the orchestrator — the original monolithic else-if block
//   has been decomposed into the helpers above.
// ====================================================================

bool SourceStreamUpdateStep(Source* source, Decoder* decoder)
{
    Stream* stream = source->stream;
    ValidateStream(stream);

    // ----- issue new read request if needed --------------------------
    if (stream->NeedsIORequest())
    {
        const int64_t  size      = stream->CalculateReadSize();
        const unsigned latencyMs = stream->CalculateReadLatency();

        stream->ioBuffer = StreamBufferRead(stream->filename,
                                            latencyMs,
                                            stream->readOffset + stream->entry->offset,
                                            size,
                                            stream->fileHandle);

        if (stream->ioBuffer)
        {
            HandleSuccessfulRead(stream, source, size);
            return true;
        }
    }

    // ----- feed ready buffers to the decoder -------------------------
    SubmitBuffersToDecoder(stream, source, decoder);

    return false;
}


// ====================================================================
// Helper: move a completed ioBuffer into the next free buffer slot.
// Called by the tick loop when IsIOBufferReady() returns true.
// ====================================================================

static void PromoteIOBuffer(Stream* stream)
{
    assert(stream->ioBuffer);
    assert(IsIOBufferReady(stream));

    // find an empty slot
    for (int i = 0; i < Stream::BUFFER_COUNT; i++)
    {
        if (stream->buffers[i] == nullptr)
        {
            stream->buffers[i]          = stream->ioBuffer;
            stream->buffersSubmitted[i] = false;
            stream->ioBuffer            = nullptr;

            printf("  [PROMO] ioBuffer -> buffers[%d]  (readSize=%lld)\n",
                   i, (long long)stream->buffers[i]->readSize);
            return;
        }
    }

    // all slots full — shouldn't happen with correct triple-buffer logic
    printf("  [WARN]  PromoteIOBuffer: all buffer slots are full!\n");
}


// ====================================================================
// ReleaseConsumedBuffers
//   Simulates the decoder finishing with a submitted buffer.
//   In a real engine the decoder would signal completion via callback;
//   here we simply free every submitted buffer after one tick.
// ====================================================================

static void ReleaseConsumedBuffers(Stream* stream, Decoder* decoder)
{
    for (int i = 0; i < Stream::BUFFER_COUNT; i++)
    {
        if (stream->buffersSubmitted[i] && stream->buffers[i])
        {
            // decoder consumed this buffer — release it
            printf("  [REL]   buffers[%d] consumed by decoder, releasing\n", i);

            delete[] stream->buffers[i]->data;
            delete   stream->buffers[i];

            stream->buffers[i]          = nullptr;
            stream->buffersSubmitted[i] = false;
        }
    }

    // reset decoder state so it can accept more data
    if (decoder->eos)
    {
        decoder->eos  = false;
        decoder->done = false;
    }
}


// ====================================================================
// tick
//   Runs one iteration of the engine update.
//   Advances mock time so async IO completes after latency.
// ====================================================================

void tick(Source* source, Decoder* decoder, unsigned deltaMs)
{
    g_mockTimeMs += deltaMs;

    Stream* stream = source->stream;

    // 1. release buffers the decoder finished with last tick
    ReleaseConsumedBuffers(stream, decoder);

    // 2. check if the in-flight IO finished — if so, promote to a slot
    if (stream->ioBuffer && IsIOBufferReady(stream))
    {
        PromoteIOBuffer(stream);
    }

    // 3. run the streaming state machine (issue IO + submit to decoder)
    SourceStreamUpdateStep(source, decoder);
}


// ====================================================================
// main
//   Self-contained demo of the streaming pipeline.
//   Simulates playing an audio file in 256 KB chunks through a
//   triple-buffered decoder loop.
// ====================================================================

int main()
{
    printf("============================================================\n");
    printf("  Streaming System Demo\n");
    printf("  STREAM_BUFFER_SIZE     = %d bytes  (%d KB)\n",
           STREAM_BUFFER_SIZE, STREAM_BUFFER_SIZE / 1024);
    printf("  STREAM_BASE_LATENCY_MS = %d ms\n", STREAM_BASE_LATENCY_MS);
    printf("  SOURCE_BUFFER_COUNT    = %d\n", SOURCE_BUFFER_COUNT);
    printf("============================================================\n\n");

    // ---- setup: describe a fake audio file --------------------------
    // 3 × STREAM_BUFFER_SIZE so we get exactly 3 reads per loop pass
    StreamEntry entry;
    entry.size         = 3 * STREAM_BUFFER_SIZE;   // 768 KB
    entry.offset       = 0;
    entry.channelCount = 2;
    entry.looping      = true;          // loop at end of file

    // ---- create the pipeline objects --------------------------------
    Stream   streamObj;
    streamObj.filename   = "music.ogg";
    streamObj.entry      = &entry;
    streamObj.fileHandle = nullptr;

    Source   sourceObj;
    sourceObj.stream = &streamObj;
    sourceObj.entry  = &entry;
    sourceObj.primed = true;

    Decoder  decoderObj;

    Source*  source  = &sourceObj;
    Decoder* decoder = &decoderObj;

    printf("Setup:  entry.size=%lld  channels=%d  looping=%s  primed=%s\n\n",
           (long long)entry.size, entry.channelCount,
           entry.looping ? "true" : "false",
           source->primed ? "true" : "false");

    // ---- run the streaming loop -------------------------------------
    //
    // Each tick:
    //   1. release buffers consumed by the decoder last tick
    //   2. promote any completed IO buffer into a slot
    //   3. issue new IO if needed + submit ready buffers to decoder
    //
    // Using 100 ms ticks so IO latency (500 ms) resolves in ~5 ticks.
    //
    const int  MAX_TICKS = 45;
    int        tickCount = 0;
    int64_t    prevReadOffset = -1;   // track wraps across ticks

    printf("--- streaming loop begin ---\n\n");

    while (tickCount < MAX_TICKS && !source->eos)
    {
        tickCount++;
        printf("[TICK %2d]  time=%u ms  readOffset=%lld\n",
               tickCount, g_mockTimeMs, (long long)streamObj.readOffset);

        tick(source, decoder, /*deltaMs=*/100);

        // detect a completed loop pass (offset jumped backwards)
        if (prevReadOffset > 0 && streamObj.readOffset == 0)
            printf("  ===== loop wrap complete =====\n");

        prevReadOffset = streamObj.readOffset;

        printf("\n");
    }

    printf("--- streaming loop end ---\n");
    printf("ticks: %d  eos: %s  decoder.eos: %s  decoder.error: %s\n",
           tickCount,
           source->eos     ? "true" : "false",
           decoder->eos    ? "true" : "false",
           decoder->error  ? "true" : "false");

    printf("\nFinal stream state:\n");
    printf("  readOffset  = %lld / %lld\n",
           (long long)streamObj.readOffset, (long long)entry.size);
    printf("  ioBuffer    = %s\n",
           streamObj.ioBuffer ? "in-flight" : "null");
    printf("  buffers[0]  = %s  submitted=%s\n",
           streamObj.buffers[0] ? "present" : "null",
           streamObj.buffersSubmitted[0] ? "true" : "false");
    printf("  buffers[1]  = %s  submitted=%s\n",
           streamObj.buffers[1] ? "present" : "null",
           streamObj.buffersSubmitted[1] ? "true" : "false");
    printf("  lastBuffer  = %s\n",
           streamObj.lastBuffer ? "set" : "null");

    // ---- cleanup ----------------------------------------------------
    for (int i = 0; i < Stream::BUFFER_COUNT; i++)
    {
        if (streamObj.buffers[i])
        {
            delete[] streamObj.buffers[i]->data;
            delete   streamObj.buffers[i];
        }
    }
    if (streamObj.ioBuffer)
    {
        delete[] streamObj.ioBuffer->data;
        delete   streamObj.ioBuffer;
    }

    printf("\nDone.\n");
    return 0;
}
