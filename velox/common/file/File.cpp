/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/common/file/File.h"

#include <fmt/format.h>
#include <glog/logging.h>
#include <memory>
#include <stdexcept>

#include <fcntl.h>
#include <folly/portability/SysUio.h>

namespace facebook::velox {

std::string_view InMemoryReadFile::pread(
    uint64_t offset,
    uint64_t length,
    Arena* /*unused_arena*/) const {
  bytesRead_ += length;
  return file_.substr(offset, length);
}

std::string_view
InMemoryReadFile::pread(uint64_t offset, uint64_t length, void* buf) const {
  bytesRead_ += length;
  memcpy(buf, file_.data() + offset, length);
  return {static_cast<char*>(buf), length};
}

std::string InMemoryReadFile::pread(uint64_t offset, uint64_t length) const {
  bytesRead_ += length;
  return std::string(file_.data() + offset, length);
}

uint64_t InMemoryReadFile::preadv(
    uint64_t offset,
    const std::vector<folly::Range<char*>>& buffers) {
  uint64_t numRead = 0;
  if (offset >= file_.size()) {
    return 0;
  }
  for (auto& range : buffers) {
    auto copySize = std::min<size_t>(range.size(), file_.size() - offset);
    if (range.data()) {
      memcpy(range.data(), file_.data() + offset, copySize);
    }
    offset += copySize;
    numRead += copySize;
  }
  return numRead;
}

void InMemoryWriteFile::append(std::string_view data) {
  file_->append(data);
}

uint64_t InMemoryWriteFile::size() const {
  return file_->size();
}

LocalReadFile::LocalReadFile(std::string_view path) {
  std::unique_ptr<char[]> buf(new char[path.size() + 1]);
  buf[path.size()] = 0;
  memcpy(buf.get(), path.data(), path.size());
  fd_ = open(buf.get(), O_RDONLY);
  if (fd_ < 0) {
    throw std::runtime_error("open failure in LocalReadFile constructor.");
  }
}

void LocalReadFile::preadInternal(uint64_t offset, uint64_t length, char* pos)
    const {
  bytesRead_ += length;
  auto bytesRead = ::pread(fd_, pos, length, offset);
  if (bytesRead != length) {
    throw std::runtime_error("fread failure in LocalReadFile::PReadInternal.");
  }
}

std::string_view
LocalReadFile::pread(uint64_t offset, uint64_t length, Arena* arena) const {
  char* pos = arena->reserve(length);
  preadInternal(offset, length, pos);
  return {pos, length};
}

std::string_view
LocalReadFile::pread(uint64_t offset, uint64_t length, void* buf) const {
  preadInternal(offset, length, static_cast<char*>(buf));
  return {static_cast<char*>(buf), length};
}

std::string LocalReadFile::pread(uint64_t offset, uint64_t length) const {
  // TODO: use allocator that doesn't initialize memory?
  std::string result(length, 0);
  char* pos = result.data();
  preadInternal(offset, length, pos);
  return result;
}

uint64_t LocalReadFile::preadv(
    uint64_t offset,
    const std::vector<folly::Range<char*>>& buffers) {
  static char droppedBytes[8 * 1024];
  std::vector<struct iovec> iovecs;
  iovecs.reserve(buffers.size());
  for (auto& range : buffers) {
    if (!range.data()) {
      auto skipSize = range.size();
      while (skipSize) {
        auto bytes = std::min<size_t>(sizeof(droppedBytes), skipSize);
        iovecs.push_back({droppedBytes, bytes});
        skipSize -= bytes;
      }
    } else {
      iovecs.push_back({range.data(), range.size()});
    }
  }
  return folly::preadv(fd_, iovecs.data(), iovecs.size(), offset);
}

uint64_t LocalReadFile::size() const {
  if (size_ != -1) {
    return size_;
  }
  const off_t rc = lseek(fd_, 0, SEEK_END);
  if (rc < 0) {
    throw std::runtime_error("fseek failure in LocalReadFile::size.");
  }
  size_ = rc;
  return size_;
}

uint64_t LocalReadFile::memoryUsage() const {
  // TODO: does FILE really not use any more memory? From the stdio.h
  // source code it looks like it has only a single integer? Probably
  // we need to go deeper and see how much system memory is being taken
  // by the file descriptor the integer refers to?
  return sizeof(FILE);
}

LocalWriteFile::LocalWriteFile(std::string_view path) {
  std::unique_ptr<char[]> buf(new char[path.size() + 1]);
  buf[path.size()] = 0;
  memcpy(buf.get(), path.data(), path.size());
  {
    FILE* exists = fopen(buf.get(), "rb");
    if (exists != nullptr) {
      throw std::runtime_error(fmt::format(
          "Failure in LocalWriteFile: path '{}' already exists.", path));
    }
  }
  file_ = fopen(buf.get(), "ab");
  if (file_ == nullptr) {
    throw std::runtime_error("fread failure in LocalWriteFile constructor.");
  }
}

LocalWriteFile::~LocalWriteFile() {
  if (file_) {
    const int fclose_ret = fclose(file_);
    if (fclose_ret != 0) {
      // We cannot throw an exception from the destructor. Warn instead.
      LOG(WARNING) << "fclose failure in LocalWriteFile destructor";
    }
  }
}

void LocalWriteFile::append(std::string_view data) {
  const uint64_t bytes_written = fwrite(data.data(), 1, data.size(), file_);
  if (bytes_written != data.size()) {
    throw std::runtime_error("fwrite failure in LocalWriteFile::append.");
  }
}

void LocalWriteFile::flush() {
  auto ret = fflush(file_);
  if (ret != 0) {
    throw std::runtime_error("fflush failed in LocalWriteFile::flush.");
  }
}

uint64_t LocalWriteFile::size() const {
  return ftell(file_);
}
} // namespace facebook::velox
