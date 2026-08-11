#pragma once

#include "src/types.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace core::diagnostics
{
enum class DiagnosticSeverity : uint8
{
	Trace,
	Information,
	Warning,
	Error,
	Fatal
};

struct Diagnostic final
{
	uint64 Sequence = 0;
	std::chrono::system_clock::time_point Timestamp;
	std::thread::id Thread;
	DiagnosticSeverity Severity = DiagnosticSeverity::Information;
	string Category;
	string Message;
};

class ENGINE_API DiagnosticSink final
{
  public:
	explicit DiagnosticSink(usize Capacity = 16'384);

	void Publish(DiagnosticSeverity Severity, string Category, string Message);
	[[nodiscard]] std::vector<Diagnostic> Consume();
	void SnapshotInto(std::vector<Diagnostic> &Result) const;
	[[nodiscard]] std::vector<Diagnostic> Snapshot() const;
	[[nodiscard]] usize GetCapacity() const noexcept;
	[[nodiscard]] usize GetDroppedCount() const;

  private:
	usize Capacity = 0;
	mutable std::mutex Mutex;
	std::deque<Diagnostic> Diagnostics;
	uint64 NextSequence = 1;
	usize DroppedCount = 0;
};
} // namespace core::diagnostics
