#pragma once

#include "src/types.h"

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

namespace editor::asset
{
class ContentWatcherException final : public std::runtime_error
{
  public:
	using std::runtime_error::runtime_error;
};

enum class ContentChangeKind : uint8
{
	Added,
	Removed,
	Modified,
	Renamed
};

struct ContentChange final
{
	ContentChangeKind Kind = ContentChangeKind::Modified;
	std::filesystem::path RelativePath;
	std::filesystem::path PreviousPath;
};

class ContentWatcher final
{
  public:
	ContentWatcher();
	~ContentWatcher();

	ContentWatcher(const ContentWatcher &) = delete;
	ContentWatcher &operator=(const ContentWatcher &) = delete;
	ContentWatcher(ContentWatcher &&) = delete;
	ContentWatcher &operator=(ContentWatcher &&) = delete;

	void Start(const std::filesystem::path &Root);
	void Stop() noexcept;

	[[nodiscard]] bool IsRunning() const noexcept;
	[[nodiscard]] uint64 GetChangeGeneration() const noexcept;
	[[nodiscard]] uint64 GetOverflowCount() const noexcept;
	[[nodiscard]] string GetDiagnostic() const;
	void DrainChangesInto(std::vector<ContentChange> &Result);

  private:
	class Implementation;
	std::unique_ptr<Implementation> State;
};
} // namespace editor::asset
