#pragma once

#include <DataTypes/IDataType.h>
#include <Common/PODArray_fwd.h>
#include <Common/Exception.h>

constexpr size_t MAX_FIXEDSTRING_SIZE = 0xFFFFFF;
constexpr size_t MAX_FIXEDSTRING_SIZE_WITHOUT_SUSPICIOUS = 256;


namespace DB
{

class ColumnFixedString;

namespace ErrorCodes
{
    extern const int ARGUMENT_OUT_OF_BOUND;
}


class DataTypeFixedString final : public IDataType
{
private:
    size_t n;

public:
    using ColumnType = ColumnFixedString;

    static constexpr bool is_parametric = true;
    static constexpr auto type_id = TypeIndex::FixedString;

    explicit DataTypeFixedString(size_t n_) : n(n_)
    {
        if (n == 0)
            throw Exception(ErrorCodes::ARGUMENT_OUT_OF_BOUND, "FixedString size must be positive");
        if (n > MAX_FIXEDSTRING_SIZE)
            throw Exception(ErrorCodes::ARGUMENT_OUT_OF_BOUND, "FixedString size is too large");
    }

    std::string doGetName() const override;
    TypeIndex getTypeId() const override { return type_id; }

    const char * getFamilyName() const override { return "FixedString"; }

    size_t getN() const
    {
        return n;
    }

    MutableColumnPtr createColumn() const override;

    Field getDefault() const override;

    bool equals(const IDataType & rhs) const override;

    SerializationPtr doGetDefaultSerialization() const override;

    bool isParametric() const override { return true; }
    bool haveSubtypes() const override { return false; }
    bool isComparable() const override { return true; }
    bool isValueUnambiguouslyRepresentedInContiguousMemoryRegion() const override { return true; }
    bool isValueUnambiguouslyRepresentedInFixedSizeContiguousMemoryRegion() const override { return true; }
    bool haveMaximumSizeOfValue() const override { return true; }
    size_t getSizeOfValueInMemory() const override { return n; }
    bool isCategorial() const override { return true; }
    bool canBeInsideNullable() const override { return true; }
    bool canBeInsideLowCardinality() const override { return true; }

    /// Makes sure that the length of a newly inserted string to `chars` is equal to getN().
    /// If the length is less than getN() the function will add zero characters up to getN().
    /// If the length is greater than getN() the function will throw an exception.
    void alignStringLength(PaddedPODArray<UInt8> & chars, size_t old_size) const;
};

}
