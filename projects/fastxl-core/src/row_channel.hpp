#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fastxl/dom.hpp"
#include "fastxl/error.hpp"

namespace fastxl::detail::table {

using PumpedCell = fastxl::Cell;

struct PumpedRow {
  uint32_t number = 0;
  std::vector<PumpedCell> cells;
};

// Bounded producer/consumer queue that lets a pull-based consumer read rows
// produced by the push-based SAX reader without buffering the sheet. The
// producer runs on its own thread; `stop()` closes the queue and joins it.
class RowChannel {
public:
  explicit RowChannel(size_t capacity) : capacity_(capacity) {}
  ~RowChannel() { stop(); }

  void start(std::function<void()> producer) {
    worker_ = std::thread([this, producer = std::move(producer)]() {
      try {
        producer();
        {
          std::lock_guard<std::mutex> lock(mutex_);
          finished_ = true;
        }
        not_empty_.notify_all();
      } catch (const Exception& e) {
        fail(e.code(), e.what());
      } catch (const std::exception& e) {
        fail(ErrorCode::Internal, e.what());
      } catch (...) {
        fail(ErrorCode::Internal, "unknown error while reading worksheet");
      }
    });
  }

  // Producer side; returns false when the consumer closed the channel.
  bool push(PumpedRow row) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_full_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
    if (closed_) return false;
    queue_.push_back(std::move(row));
    not_empty_.notify_one();
    return true;
  }

  // Consumer side; returns false at end of input.
  bool pop(PumpedRow& row) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this] { return closed_ || finished_ || !queue_.empty(); });
    if (queue_.empty()) {
      if (has_error_) throw Exception(error_.first, error_.second);
      return false;
    }
    row = std::move(queue_.front());
    queue_.pop_front();
    not_full_.notify_one();
    return true;
  }

  // Consumer side: stop the producer and drain.
  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    not_full_.notify_all();
    not_empty_.notify_all();
  }

  void join() {
    if (worker_.joinable()) worker_.join();
  }

  void stop() {
    close();
    join();
  }

private:
  void fail(ErrorCode code, std::string message) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!has_error_) {
        error_ = std::make_pair(code, std::move(message));
        has_error_ = true;
      }
      finished_ = true;
    }
    not_empty_.notify_all();
  }

  size_t capacity_;
  std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<PumpedRow> queue_;
  bool closed_ = false;
  bool finished_ = false;
  std::pair<ErrorCode, std::string> error_{};
  bool has_error_ = false;
  std::thread worker_;
};

} // namespace fastxl::detail::table
