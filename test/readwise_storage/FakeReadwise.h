#pragma once

// In-memory fakes standing in for the SD card and the network.
//
// The store can be told to fail the Nth write, which is the only way to test
// issue #3's "interrupted writes retain the previous valid data and checkpoint"
// without pulling the power on a real device mid-sync.

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "lib/Readwise/ReadwiseApi.h"
#include "lib/Readwise/ReadwiseCodec.h"
#include "lib/Readwise/ReadwiseFileStore.h"

namespace testing_support {

class FakeFileStore : public readwise::ReadwiseFileStore {
 public:
  // Every whole-file replace and every commit counts as one durable write. Set
  // to N to make the Nth such operation fail, simulating a power loss.
  void failAtWrite(int n) { failAt_ = n; }
  int writeCount() const { return writeCount_; }

  const std::map<std::string, std::vector<uint8_t>>& files() const { return files_; }
  bool has(const std::string& path) const { return files_.count(path) != 0; }

  void put(const std::string& path, const std::vector<uint8_t>& data) { files_[path] = data; }

  bool writeAll(const std::string& path, const uint8_t* data, size_t len) override {
    if (shouldFail()) {
      return false;
    }
    files_[path].assign(data, data + len);
    return true;
  }

  int readRange(const std::string& path, size_t offset, uint8_t* buf, size_t bufCap) override {
    auto it = files_.find(path);
    if (it == files_.end() || offset > it->second.size()) {
      return -1;
    }
    const size_t available = it->second.size() - offset;
    const size_t toCopy = available < bufCap ? available : bufCap;
    memcpy(buf, it->second.data() + offset, toCopy);
    return static_cast<int>(toCopy);
  }

  bool exists(const std::string& path) override { return files_.count(path) != 0; }

  bool remove(const std::string& path) override { return files_.erase(path) != 0; }

  long size(const std::string& path) override {
    auto it = files_.find(path);
    return it == files_.end() ? -1 : static_cast<long>(it->second.size());
  }

  bool ensureDir(const std::string&) override { return true; }

  bool beginWrite(const std::string& path) override {
    if (writeOpen_) {
      return false;
    }
    writeOpen_ = true;
    pendingPath_ = path;
    pending_.clear();
    return true;
  }

  bool writeChunk(const uint8_t* data, size_t len) override {
    if (!writeOpen_) {
      return false;
    }
    pending_.insert(pending_.end(), data, data + len);
    return true;
  }

  bool patchWrite(size_t offset, const uint8_t* data, size_t len) override {
    if (!writeOpen_ || offset + len > pending_.size()) {
      return false;
    }
    memcpy(pending_.data() + offset, data, len);
    return true;
  }

  // The commit is the only point at which the destination changes, mirroring
  // the temp-then-rename the SD implementation performs.
  bool commitWrite() override {
    if (!writeOpen_) {
      return false;
    }
    writeOpen_ = false;
    if (shouldFail()) {
      pending_.clear();
      return false;
    }
    files_[pendingPath_] = pending_;
    pending_.clear();
    return true;
  }

  void abortWrite() override {
    writeOpen_ = false;
    pending_.clear();
  }

 private:
  bool shouldFail() {
    ++writeCount_;
    return failAt_ > 0 && writeCount_ == failAt_;
  }

  std::map<std::string, std::vector<uint8_t>> files_;
  std::vector<uint8_t> pending_;
  std::string pendingPath_;
  bool writeOpen_ = false;
  int failAt_ = 0;
  int writeCount_ = 0;
};

class FakeApi : public readwise::ReadwiseApi {
 public:
  struct Page {
    std::vector<readwise::Document> documents;
    std::string nextCursor;
    readwise::ApiStatus status = readwise::ApiStatus::Ok;
  };

  // Pages are served in order regardless of location, which keeps the tests
  // focused on pipeline behaviour rather than query construction.
  std::vector<Page> pages;
  size_t pageIndex = 0;

  readwise::ApiStatus pushStatus = readwise::ApiStatus::Ok;
  // Fails the Nth push (1-based); 0 disables.
  int failPushAt = 0;
  std::vector<readwise::PendingOp> pushed;
  std::vector<readwise::ListQuery> queries;

  readwise::ListResponse fetchPage(const readwise::ListQuery& query, readwise::DocumentSink& sink) override {
    queries.push_back(query);
    readwise::ListResponse response;
    if (pageIndex >= pages.size()) {
      return response;  // Ok with an empty cursor: no more pages.
    }
    const Page& page = pages[pageIndex++];
    if (page.status != readwise::ApiStatus::Ok) {
      response.status = page.status;
      response.retryAfterSeconds = 16;
      return response;
    }
    for (const readwise::Document& doc : page.documents) {
      if (!sink.onDocument(doc)) {
        break;
      }
    }
    readwise::copyBounded(response.nextPageCursor, readwise::CURSOR_CAP, page.nextCursor.c_str(),
                          page.nextCursor.size());
    return response;
  }

  readwise::ApiStatus pushOp(const readwise::PendingOp& op) override {
    ++pushAttempts;
    if (failPushAt > 0 && pushAttempts == failPushAt) {
      return pushStatus == readwise::ApiStatus::Ok ? readwise::ApiStatus::NetworkError : pushStatus;
    }
    pushed.push_back(op);
    return readwise::ApiStatus::Ok;
  }

  // Bodies keyed by document id, delivered in two chunks to exercise the
  // chunked contract.
  std::map<std::string, std::string> bodies;
  readwise::ApiStatus fetchBody(const char* id, readwise::BodySink& sink, uint16_t* retryAfterSeconds) override {
    if (retryAfterSeconds != nullptr) {
      *retryAfterSeconds = 0;
    }
    auto it = bodies.find(id != nullptr ? id : "");
    if (it == bodies.end()) {
      return readwise::ApiStatus::ServerError;
    }
    const std::string& body = it->second;
    const size_t half = body.size() / 2;
    if (half > 0 && !sink.onBodyChunk(body.data(), half)) {
      return readwise::ApiStatus::ParseError;
    }
    if (!sink.onBodyChunk(body.data() + half, body.size() - half)) {
      return readwise::ApiStatus::ParseError;
    }
    sink.onBodyEnd(true);
    return readwise::ApiStatus::Ok;
  }

  int pushAttempts = 0;
};

inline readwise::Document makeDoc(const char* id, readwise::Location location, const char* updatedAt,
                                  const char* lastMovedAt) {
  readwise::Document doc;
  readwise::copyBounded(doc.id, readwise::ID_CAP, id, strlen(id));
  readwise::copyBounded(doc.title, readwise::TITLE_CAP, "Title", 5);
  readwise::copyBounded(doc.updatedAt, readwise::TIMESTAMP_CAP, updatedAt, strlen(updatedAt));
  readwise::copyBounded(doc.lastMovedAt, readwise::TIMESTAMP_CAP, lastMovedAt, strlen(lastMovedAt));
  doc.location = location;
  doc.category = readwise::Category::Article;
  return doc;
}

}  // namespace testing_support
