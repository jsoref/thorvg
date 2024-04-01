/*
 * Copyright (c) 2020 - 2024 the ThorVG project. All rights reserved.

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Lempel–Ziv–Welch (LZW) encoder/decoder by Guilherme R. Lampert(guilherme.ronaldo.lampert@gmail.com)

 * This is the compression scheme used by the GIF image format and the Unix 'compress' tool.
 * Main differences from this implementation is that End Of Input (EOI) and Clear Codes (CC)
 * are not stored in the output and the max code length in bits is 12, vs 16 in compress.
 *
 * EOI is simply detected by the end of the data stream, while CC happens if the
 * dictionary gets filled. Data is written/read from bit streams, which handle
 * byte-alignment for us in a transparent way.

 * The decoder relies on the hardcoded data layout produced by the encoder, since
 * no additional reconstruction data is added to the output, so they must match.
 * The nice thing about LZW is that we can reconstruct the dictionary directly from
 * the stream of codes generated by the encoder, so this avoids storing additional
 * headers in the bit stream.

 * The output code length is variable. It starts with the minimum number of bits
 * required to store the base byte-sized dictionary and automatically increases
 * as the dictionary gets larger (it starts at 9-bits and grows to 10-bits when
 * code 512 is added, then 11-bits when 1024 is added, and so on). If the dictionary
 * is filled (4096 items for a 12-bits dictionary), the whole thing is cleared and
 * the process starts over. This is the main reason why the encoder and the decoder
 * must match perfectly, since the lengths of the codes will not be specified with
 * the data itself.

 * USEFUL LINKS:
 * https://en.wikipedia.org/wiki/Lempel%E2%80%93Ziv%E2%80%93Welch
 * http://rosettacode.org/wiki/LZW_compression
 * http://www.cs.duke.edu/csed/curious/compression/lzw.html
 * http://www.cs.cf.ac.uk/Dave/Multimedia/node214.html
 * http://marknelson.us/1989/10/01/lzw-data-compression/
 */
#include "config.h"



#include <string>
#include <memory.h>
#include "tvgCompressor.h"

namespace tvg {


/************************************************************************/
/* LZW Implementation                                                   */
/************************************************************************/


//LZW Dictionary helper:
constexpr int Nil = -1;
constexpr int MaxDictBits = 12;
constexpr int StartBits = 9;
constexpr int FirstCode = (1 << (StartBits - 1)); // 256
constexpr int MaxDictEntries = (1 << MaxDictBits);     // 4096


//Round up to the next power-of-two number, e.g. 37 => 64
static int nextPowerOfTwo(int num)
{
    --num;
    for (size_t i = 1; i < sizeof(num) * 8; i <<= 1) {
        num = num | num >> i;
    }
    return ++num;
}


struct BitStreamWriter
{
    uint8_t* stream;       //Growable buffer to store our bits. Heap allocated & owned by the class instance.
    int bytesAllocated;    //Current size of heap-allocated stream buffer *in bytes*.
    int granularity;       //Amount bytesAllocated multiplies by when auto-resizing in appendBit().
    int currBytePos;       //Current byte being written to, from 0 to bytesAllocated-1.
    int nextBitPos;        //Bit position within the current byte to access next. 0 to 7.
    int numBitsWritten;    //Number of bits in use from the stream buffer, not including byte-rounding padding.

    void internalInit()
    {
        stream = nullptr;
        bytesAllocated = 0;
        granularity = 2;
        currBytePos = 0;
        nextBitPos = 0;
        numBitsWritten = 0;
    }

    uint8_t* allocBytes(const int bytesWanted, uint8_t * oldPtr, const int oldSize)
    {
        auto newMemory = static_cast<uint8_t *>(malloc(bytesWanted));
        memset(newMemory, 0, bytesWanted);

        if (oldPtr) {
            memcpy(newMemory, oldPtr, oldSize);
            free(oldPtr);
        }
        return newMemory;
    }

    BitStreamWriter()
    {
        /* 8192 bits for a start (1024 bytes). It will resize if needed.
           Default granularity is 2. */
        internalInit();
        allocate(8192);
    }

    BitStreamWriter(const int initialSizeInBits, const int growthGranularity = 2)
    {
        internalInit();
        setGranularity(growthGranularity);
        allocate(initialSizeInBits);
    }

    ~BitStreamWriter()
    {
        free(stream);
    }

    void allocate(int bitsWanted)
    {
        //Require at least a byte.
        if (bitsWanted <= 0) bitsWanted = 8;

        //Round upwards if needed:
        if ((bitsWanted % 8) != 0) bitsWanted = nextPowerOfTwo(bitsWanted);

        //We might already have the required count.
        const int sizeInBytes = bitsWanted / 8;
        if (sizeInBytes <= bytesAllocated) return;

        stream = allocBytes(sizeInBytes, stream, bytesAllocated);
        bytesAllocated = sizeInBytes;
    }

    void appendBit(const int bit)
    {
        const uint32_t mask = uint32_t(1) << nextBitPos;
        stream[currBytePos] = (stream[currBytePos] & ~mask) | (-bit & mask);
        ++numBitsWritten;

        if (++nextBitPos == 8) {
            nextBitPos = 0;
            if (++currBytePos == bytesAllocated) allocate(bytesAllocated * granularity * 8);
        }
    }

    void appendBitsU64(const uint64_t num, const int bitCount)
    {
        for (int b = 0; b < bitCount; ++b) {
            const uint64_t mask = uint64_t(1) << b;
            const int bit = !!(num & mask);
            appendBit(bit);
        }
    }

    uint8_t* release()
    {
        auto oldPtr = stream;
        internalInit();
        return oldPtr;
    }

    void setGranularity(const int growthGranularity)
    {
        granularity = (growthGranularity >= 2) ? growthGranularity : 2;
    }

    int getByteCount() const
    {
        int usedBytes = numBitsWritten / 8;
        int leftovers = numBitsWritten % 8;
        if (leftovers != 0) ++usedBytes;
        return usedBytes;
    }
};


struct BitStreamReader
{
    const uint8_t* stream;       // Pointer to the external bit stream. Not owned by the reader.
    const int sizeInBytes;       // Size of the stream *in bytes*. Might include padding.
    const int sizeInBits;        // Size of the stream *in bits*, padding *not* include.
    int currBytePos = 0;         // Current byte being read in the stream.
    int nextBitPos = 0;          // Bit position within the current byte to access next. 0 to 7.
    int numBitsRead = 0;         // Total bits read from the stream so far. Never includes byte-rounding padding.

    BitStreamReader(const uint8_t* bitStream, const int byteCount, const int bitCount) : stream(bitStream), sizeInBytes(byteCount), sizeInBits(bitCount)
    {
    }

    bool readNextBit(int& bitOut)
    {
        if (numBitsRead >= sizeInBits) return false; //We are done.

        const uint32_t mask = uint32_t(1) << nextBitPos;
        bitOut = !!(stream[currBytePos] & mask);
        ++numBitsRead;

        if (++nextBitPos == 8) {
            nextBitPos = 0;
            ++currBytePos;
        }
        return true;
    }

    uint64_t readBitsU64(const int bitCount)
    {
        uint64_t num = 0;
        for (int b = 0; b < bitCount; ++b) {
            int bit;
            if (!readNextBit(bit)) break;
            /* Based on a "Stanford bit-hack":
               http://graphics.stanford.edu/~seander/bithacks.html#ConditionalSetOrClearBitsWithoutBranching */
            const uint64_t mask = uint64_t(1) << b;
            num = (num & ~mask) | (-bit & mask);
        }
        return num;
    }

    bool isEndOfStream() const
    {
        return numBitsRead >= sizeInBits;
    }
};


struct Dictionary
{
    struct Entry
    {
        int code;
        int value;
    };

    //Dictionary entries 0-255 are always reserved to the byte/ASCII range.
    int size;
    Entry entries[MaxDictEntries];

    Dictionary()
    {
        /* First 256 dictionary entries are reserved to the byte/ASCII range.
           Additional entries follow for the character sequences found in the input.
           Up to 4096 - 256 (MaxDictEntries - FirstCode). */
        size = FirstCode;

        for (int i = 0; i < size; ++i) {
            entries[i].code  = Nil;
            entries[i].value = i;
        }
    }

    int findIndex(const int code, const int value) const
    {
        if (code == Nil) return value;

        //Linear search for now.
        //TODO: Worth optimizing with a proper hash-table?
        for (int i = 0; i < size; ++i) {
            if (entries[i].code == code && entries[i].value == value) return i;
        }
        return Nil;
    }

    bool add(const int code, const int value)
    {
        if (size == MaxDictEntries) return false;
        entries[size].code  = code;
        entries[size].value = value;
        ++size;
        return true;
    }

    bool flush(int & codeBitsWidth)
    {
        if (size == (1 << codeBitsWidth)) {
            ++codeBitsWidth;
            if (codeBitsWidth > MaxDictBits) {
                //Clear the dictionary (except the first 256 byte entries).
                codeBitsWidth = StartBits;
                size = FirstCode;
                return true;
            }
        }
        return false;
    }
};


static bool outputByte(int code, uint8_t*& output, int outputSizeBytes, int& bytesDecodedSoFar)
{
    if (bytesDecodedSoFar >= outputSizeBytes) return false;
    *output++ = static_cast<uint8_t>(code);
    ++bytesDecodedSoFar;
    return true;
}


static bool outputSequence(const Dictionary& dict, int code, uint8_t*& output, int outputSizeBytes, int& bytesDecodedSoFar, int& firstByte)
{
    /* A sequence is stored backwards, so we have to write
       it to a temp then output the buffer in reverse. */
    int i = 0;
    uint8_t sequence[MaxDictEntries];

    do {
        sequence[i++] = dict.entries[code].value;
        code = dict.entries[code].code;
    } while (code >= 0);

    firstByte = sequence[--i];

    for (; i >= 0; --i) {
        if (!outputByte(sequence[i], output, outputSizeBytes, bytesDecodedSoFar)) return false;
    }
    return true;
}


uint8_t* lzwDecode(const uint8_t* compressed, uint32_t compressedSizeBytes, uint32_t compressedSizeBits, uint32_t uncompressedSizeBytes)
{
    int code = Nil;
    int prevCode = Nil;
    int firstByte = 0;
    int bytesDecoded = 0;
    int codeBitsWidth = StartBits;
    auto uncompressed = (uint8_t*) malloc(sizeof(uint8_t) * uncompressedSizeBytes);
    auto ptr = uncompressed;

    /* We'll reconstruct the dictionary based on the bit stream codes.
       Unlike Huffman encoding, we don't store the dictionary as a prefix to the data. */
    Dictionary dictionary;
    BitStreamReader bitStream(compressed, compressedSizeBytes, compressedSizeBits);

    /* We check to avoid an overflow of the user buffer.
       If the buffer is smaller than the decompressed size, we break the loop and return the current decompression count. */
    while (!bitStream.isEndOfStream()) {
        code = static_cast<int>(bitStream.readBitsU64(codeBitsWidth));

        if (prevCode == Nil) {
            if (!outputByte(code, ptr, uncompressedSizeBytes, bytesDecoded)) break;
            firstByte = code;
            prevCode  = code;
            continue;
        }
        if (code >= dictionary.size) {
            if (!outputSequence(dictionary, prevCode, ptr, uncompressedSizeBytes, bytesDecoded, firstByte)) break;
            if (!outputByte(firstByte, ptr, uncompressedSizeBytes, bytesDecoded)) break;
        } else if (!outputSequence(dictionary, code, ptr, uncompressedSizeBytes, bytesDecoded, firstByte)) break;

        dictionary.add(prevCode, firstByte);
        if (dictionary.flush(codeBitsWidth)) prevCode = Nil;
        else prevCode = code;
    }

    return uncompressed;
}


uint8_t* lzwEncode(const uint8_t* uncompressed, uint32_t uncompressedSizeBytes, uint32_t* compressedSizeBytes, uint32_t* compressedSizeBits)
{
    //LZW encoding context:
    int code = Nil;
    int codeBitsWidth = StartBits;
    Dictionary dictionary;

    //Output bit stream we write to. This will allocate memory as needed to accommodate the encoded data.
    BitStreamWriter bitStream;

    for (; uncompressedSizeBytes > 0; --uncompressedSizeBytes, ++uncompressed) {
        const int value = *uncompressed;
        const int index = dictionary.findIndex(code, value);

        if (index != Nil) {
            code = index;
            continue;
        }

        //Write the dictionary code using the minimum bit-with:
        bitStream.appendBitsU64(code, codeBitsWidth);

        //Flush it when full so we can restart the sequences.
        if (!dictionary.flush(codeBitsWidth)) {
            //There's still space for this sequence.
            dictionary.add(code, value);
        }
        code = value;
    }

    //Residual code at the end:
    if (code != Nil) bitStream.appendBitsU64(code, codeBitsWidth);

    //Pass ownership of the compressed data buffer to the user pointer:
    *compressedSizeBytes = bitStream.getByteCount();
    *compressedSizeBits = bitStream.numBitsWritten;

    return bitStream.release();
}


/************************************************************************/
/* B64 Implementation                                                   */
/************************************************************************/


size_t b64Decode(const char* encoded, const size_t len, char** decoded)
{
    static constexpr const char B64_INDEX[256] =
    {
        0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,  0,  0,  0,  0,  0,  0,  62, 63, 62, 62, 63, 52, 53, 54, 55, 56, 57,
        58, 59, 60, 61, 0,  0,  0,  0,  0,  0,  0,  0,  1,  2,  3,  4,  5,  6,
        7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
        25, 0,  0,  0,  0,  63, 0,  26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
        37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51
    };


    if (!decoded || !encoded || len == 0) return 0;

    auto reserved = 3 * (1 + (len >> 2)) + 1;
    auto output = static_cast<char*>(malloc(reserved * sizeof(char)));
    if (!output) return 0;
    output[reserved - 1] = '\0';

    size_t idx = 0;

    while (*encoded && *(encoded + 1)) {
        if (*encoded <= 0x20) {
            ++encoded;
            continue;
        }

        auto value1 = B64_INDEX[(size_t)encoded[0]];
        auto value2 = B64_INDEX[(size_t)encoded[1]];
        output[idx++] = (value1 << 2) + ((value2 & 0x30) >> 4);

        if (!encoded[2] || encoded[3] < 0 || encoded[2] == '=' || encoded[2] == '.') break;
        auto value3 = B64_INDEX[(size_t)encoded[2]];
        output[idx++] = ((value2 & 0x0f) << 4) + ((value3 & 0x3c) >> 2);

        if (!encoded[3] || encoded[3] < 0 || encoded[3] == '=' || encoded[3] == '.') break;
        auto value4 = B64_INDEX[(size_t)encoded[3]];
        output[idx++] = ((value3 & 0x03) << 6) + value4;
        encoded += 4;
    }
    *decoded = output;
    return reserved;
}


}