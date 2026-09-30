/* SPDX-License-Identifier: GPL-2.0-or-later */
// Controlled memory/byte-buffer dependencies for BitstreamConverter.cpp. The
// production NAL traversal, layer assembly and RPU processing are compiled whole.
#include "cores/FFmpeg.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C"
{
void* av_malloc(size_t size) { return std::malloc(size); }
void* av_realloc(void* ptr, size_t size) { return std::realloc(ptr, size); }
void av_free(void* ptr) { std::free(ptr); }
void av_freep(void* ptr)
{
  void* value;
  std::memcpy(&value, ptr, sizeof(value));
  std::free(value);
  value = nullptr;
  std::memcpy(ptr, &value, sizeof(value));
}

int avio_open_dyn_buf(AVIOContext** context)
{
  *context = reinterpret_cast<AVIOContext*>(new std::vector<uint8_t>);
  return 0;
}
void avio_write(AVIOContext* context, const unsigned char* bytes, int size)
{
  auto& buffer = *reinterpret_cast<std::vector<uint8_t>*>(context);
  buffer.insert(buffer.end(), bytes, bytes + size);
}
void avio_w8(AVIOContext* context, int value)
{
  const uint8_t byte = value;
  avio_write(context, &byte, 1);
}
void avio_wb16(AVIOContext* context, unsigned int value)
{
  avio_w8(context, value >> 8);
  avio_w8(context, value);
}
void avio_wb32(AVIOContext* context, unsigned int value)
{
  avio_wb16(context, value >> 16);
  avio_wb16(context, value);
}
int avio_close_dyn_buf(AVIOContext* context, uint8_t** bytes)
{
  auto* buffer = reinterpret_cast<std::vector<uint8_t>*>(context);
  const int size = buffer->size();
  *bytes = static_cast<uint8_t*>(std::calloc(1, size + AV_INPUT_BUFFER_PADDING_SIZE));
  std::memcpy(*bytes, buffer->data(), size);
  delete buffer;
  return size;
}
}

FFmpegExtraData::FFmpegExtraData(const uint8_t* bytes, size_t size)
  : m_data(static_cast<uint8_t*>(std::calloc(1, size + AV_INPUT_BUFFER_PADDING_SIZE))), m_size(size)
{
  if (size)
    std::memcpy(m_data, bytes, size);
}
FFmpegExtraData::~FFmpegExtraData() { std::free(m_data); }
FFmpegExtraData& FFmpegExtraData::operator=(FFmpegExtraData&& other) noexcept
{
  std::swap(m_data, other.m_data);
  std::swap(m_size, other.m_size);
  return *this;
}

bool StringUtils::EqualsNoCase(std::string_view a, std::string_view b) noexcept
{
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
      [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}
