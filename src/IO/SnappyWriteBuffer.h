#pragma once

#include "config.h"

#if USE_SNAPPY
#include <IO/BufferWithOwnMemory.h>
#include <IO/WriteBuffer.h>

namespace DB
{
/// Performs compression using snappy library and write compressed data to the underlying buffer.
class SnappyWriteBuffer : public BufferWithOwnMemory<WriteBuffer>
{
    using Base = BufferWithOwnMemory<WriteBuffer>;
public:
    explicit SnappyWriteBuffer(
        std::unique_ptr<WriteBuffer> out_,
        size_t buf_size = DBMS_DEFAULT_BUFFER_SIZE,
        char * existing_memory = nullptr,
        size_t alignment = 0);

    explicit SnappyWriteBuffer(
        WriteBuffer & out_,
        size_t buf_size = DBMS_DEFAULT_BUFFER_SIZE,
        char * existing_memory = nullptr,
        size_t alignment = 0);

    void finalizeImpl() override;

private:
    void nextImpl() override;

    void finishImpl();
    void finish();

    void cancelImpl() noexcept override;

    WriteBuffer * out;
    std::unique_ptr<WriteBuffer> out_holder;

    String uncompress_buffer;
    String compress_buffer;
};

}

#endif
