/*
 * Copyright (c) 2020, the SerenityOS developers.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <LibCompress/Gzip.h>

#include <AK/String.h>

namespace Compress {

bool GzipDecompressor::BlockHeader::valid_magic_number() const
{
    return identification_1 == 0x1f && identification_2 == 0x8b;
}

bool GzipDecompressor::BlockHeader::supported_by_implementation() const
{
    if (compression_method != 0x08) {
        // RFC 1952 does not define any compression methods other than deflate.
        return false;
    }

    if (flags > Flags::MAX) {
        // RFC 1952 does not define any more flags.
        return false;
    }

    if (flags & Flags::FHCRC) {
        TODO();
    }

    return true;
}

GzipDecompressor::GzipDecompressor(InputStream& stream)
    : m_input_stream(stream)
{
}

GzipDecompressor::~GzipDecompressor()
{
    m_current_member.clear();
}

// FIXME: Again, there are surely a ton of bugs because the code doesn't check for read errors.
size_t GzipDecompressor::read(Bytes bytes)
{
    if (m_current_member.has_value()) {
        size_t nread = current_member().m_stream.read(bytes);
        current_member().m_checksum.update(bytes.trim(nread));
        current_member().m_nread += nread;

        if (nread < bytes.size()) {
            LittleEndian<u32> crc32, input_size;
            m_input_stream >> crc32 >> input_size;

            if (crc32 != current_member().m_checksum.digest()) {
                m_error = true;
                return 0;
            }

            if (input_size != current_member().m_nread) {
                m_error = true;
                return 0;
            }

            m_current_member.clear();

            return nread + read(bytes.slice(nread));
        }

        return nread;
    } else {
        if (m_input_stream.eof())
            return 0;

        // FIXME: This fails with the new changes?
        BlockHeader header;
        m_input_stream >> Bytes { &header, sizeof(header) };

        if (!header.valid_magic_number() || !header.supported_by_implementation()) {
            m_error = true;
            return 0;
        }

        if (header.flags & Flags::FEXTRA) {
            LittleEndian<u16> subfield_id, length;
            m_input_stream >> subfield_id >> length;
            m_input_stream.discard_or_error(length);
        }

        if (header.flags & Flags::FNAME) {
            String original_filename;
            m_input_stream >> original_filename;
        }

        if (header.flags & Flags::FCOMMENT) {
            String comment;
            m_input_stream >> comment;
        }

        m_current_member.emplace(header, m_input_stream);
        return read(bytes);
    }
}

bool GzipDecompressor::read_or_error(Bytes bytes)
{
    if (read(bytes) < bytes.size()) {
        m_error = true;
        return false;
    }

    return true;
}

bool GzipDecompressor::discard_or_error(size_t count)
{
    u8 buffer[4096];

    size_t ndiscarded = 0;
    while (ndiscarded < count) {
        if (eof()) {
            m_error = true;
            return false;
        }

        ndiscarded += read({ buffer, min<size_t>(count - ndiscarded, sizeof(buffer)) });
    }

    return true;
}

ByteBuffer GzipDecompressor::decompress_all(ReadonlyBytes bytes)
{
    InputMemoryStream memory_stream { bytes };
    GzipDecompressor gzip_stream { memory_stream };

    auto buffer = ByteBuffer::create_uninitialized(4096);

    size_t nread = 0;
    while (!gzip_stream.eof()) {
        nread += gzip_stream.read(buffer.bytes().slice(nread));

        if (buffer.size() - nread < 4096)
            buffer.grow(buffer.size() + 4096);
    }

    buffer.trim(nread);
    return buffer;
}

bool GzipDecompressor::eof() const
{
    if (m_current_member.has_value()) {
        // FIXME: There is an ugly edge case where we read the whole deflate block
        //        but haven't read CRC32 and ISIZE.
        return current_member().m_stream.eof() && m_input_stream.eof();
    } else {
        return m_input_stream.eof();
    }
}

}
