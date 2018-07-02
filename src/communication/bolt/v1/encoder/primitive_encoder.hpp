#pragma once

#include <string>

#include "communication/bolt/v1/codes.hpp"
#include "utils/bswap.hpp"
#include "utils/cast.hpp"

namespace communication::bolt {

/**
 * Bolt PrimitiveEncoder. Has public interfaces for writing Bolt encoded data.
 * Supported types are: Null, Bool, Int, Double and String.
 *
 * Bolt encoding is used both for streaming data to network clients and for
 * database durability.
 *
 * @tparam Buffer the output buffer that should be used
 */
template <typename Buffer>
class PrimitiveEncoder {
 public:
  explicit PrimitiveEncoder(Buffer &buffer) : buffer_(buffer) {}

  void WriteRAW(const uint8_t *data, uint64_t len) { buffer_.Write(data, len); }

  void WriteRAW(const char *data, uint64_t len) {
    WriteRAW((const uint8_t *)data, len);
  }

  void WriteRAW(const uint8_t data) { WriteRAW(&data, 1); }

  template <class T>
  void WriteValue(T value) {
    value = utils::Bswap(value);
    WriteRAW(reinterpret_cast<const uint8_t *>(&value), sizeof(value));
  }

  void WriteNull() { WriteRAW(utils::UnderlyingCast(Marker::Null)); }

  void WriteBool(const bool &value) {
    if (value)
      WriteRAW(utils::UnderlyingCast(Marker::True));
    else
      WriteRAW(utils::UnderlyingCast(Marker::False));
  }

  void WriteInt(const int64_t &value) {
    if (value >= -16L && value < 128L) {
      WriteRAW(static_cast<uint8_t>(value));
    } else if (value >= -128L && value < -16L) {
      WriteRAW(utils::UnderlyingCast(Marker::Int8));
      WriteRAW(static_cast<uint8_t>(value));
    } else if (value >= -32768L && value < 32768L) {
      WriteRAW(utils::UnderlyingCast(Marker::Int16));
      WriteValue(static_cast<int16_t>(value));
    } else if (value >= -2147483648L && value < 2147483648L) {
      WriteRAW(utils::UnderlyingCast(Marker::Int32));
      WriteValue(static_cast<int32_t>(value));
    } else {
      WriteRAW(utils::UnderlyingCast(Marker::Int64));
      WriteValue(value);
    }
  }

  void WriteDouble(const double &value) {
    WriteRAW(utils::UnderlyingCast(Marker::Float64));
    WriteValue(*reinterpret_cast<const int64_t *>(&value));
  }

  void WriteTypeSize(const size_t size, const uint8_t typ) {
    if (size <= 15) {
      uint8_t len = size;
      len &= 0x0F;
      WriteRAW(utils::UnderlyingCast(MarkerTiny[typ]) + len);
    } else if (size <= 255) {
      uint8_t len = size;
      WriteRAW(utils::UnderlyingCast(Marker8[typ]));
      WriteRAW(len);
    } else if (size <= 65535) {
      uint16_t len = size;
      WriteRAW(utils::UnderlyingCast(Marker16[typ]));
      WriteValue(len);
    } else {
      uint32_t len = size;
      WriteRAW(utils::UnderlyingCast(Marker32[typ]));
      WriteValue(len);
    }
  }

  void WriteString(const std::string &value) {
    WriteTypeSize(value.size(), MarkerString);
    WriteRAW(value.c_str(), value.size());
  }

 protected:
  Buffer &buffer_;
};

}  // namespace communication::bolt
