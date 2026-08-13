#include "DiagnosticSink.h"

#include <stdexcept>
#include <utility>

namespace core::diagnostics
{
DiagnosticSink::DiagnosticSink(const usize Capacity) : Capacity(Capacity)
{
	if (Capacity == 0)
		throw std::invalid_argument("DiagnosticSink capacity must be non-zero");
}

void DiagnosticSink::Publish(const DiagnosticSeverity Severity, string Category, string Message)
{
	if (Category.empty())
		throw std::invalid_argument("Diagnostic category cannot be empty");
	if (Message.empty())
		throw std::invalid_argument("Diagnostic message cannot be empty");

	std::scoped_lock Lock(this->Mutex);
	if (this->Diagnostics.size() == this->Capacity)
	{
		this->Diagnostics.pop_front();
		++this->DroppedCount;
	}
	this->Diagnostics.push_back({.Sequence = this->NextSequence++,
								 .Timestamp = std::chrono::system_clock::now(),
								 .Thread = std::this_thread::get_id(),
								 .Severity = Severity,
								 .Category = std::move(Category),
								 .Message = std::move(Message)});
	if (this->NextSequence == 0)
		this->NextSequence = 1;
}

std::vector<Diagnostic> DiagnosticSink::Consume()
{
	std::scoped_lock Lock(this->Mutex);
	std::vector<Diagnostic> Result;
	Result.reserve(this->Diagnostics.size());
	while (!this->Diagnostics.empty())
	{
		Result.push_back(std::move(this->Diagnostics.front()));
		this->Diagnostics.pop_front();
	}
	return Result;
}

void DiagnosticSink::SnapshotInto(std::vector<Diagnostic> &Result) const
{
	std::scoped_lock Lock(this->Mutex);
	Result.clear();
	Result.reserve(this->Diagnostics.size());
	Result.insert(Result.end(), this->Diagnostics.begin(), this->Diagnostics.end());
}
std::vector<Diagnostic> DiagnosticSink::Snapshot() const
{
	std::vector<Diagnostic> Result;
	this->SnapshotInto(Result);
	return Result;
}

usize DiagnosticSink::GetCapacity() const noexcept
{
	return this->Capacity;
}

usize DiagnosticSink::GetDroppedCount() const
{
	std::scoped_lock Lock(this->Mutex);
	return this->DroppedCount;
}
} // namespace core::diagnostics
