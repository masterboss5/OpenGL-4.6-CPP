#pragma once

#include "src/types.h"

#include <compare>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace core
{
enum class DialogStatus : uint8
{
	Accepted,
	Cancelled
};

template <typename Value> struct DialogResult final
{
	DialogStatus Status = DialogStatus::Cancelled;
	std::optional<Value> Value;

	[[nodiscard]] bool Accepted() const noexcept
	{
		return this->Status == DialogStatus::Accepted && this->Value.has_value();
	}
};

class SecureBuffer final
{
  public:
	SecureBuffer() = default;
	explicit SecureBuffer(std::span<const uint8> Bytes);
	~SecureBuffer();
	SecureBuffer(const SecureBuffer &) = delete;
	SecureBuffer &operator=(const SecureBuffer &) = delete;
	SecureBuffer(SecureBuffer &&Other) noexcept;
	SecureBuffer &operator=(SecureBuffer &&Other) noexcept;

	[[nodiscard]] std::span<const uint8> Bytes() const noexcept;
	[[nodiscard]] bool Empty() const noexcept;
	void Clear() noexcept;

  private:
	std::unique_ptr<uint8[]> Memory;
	usize ByteCount = 0;
};

enum class FileDialogOperation : uint8
{
	OpenFile,
	OpenFiles,
	SaveFile,
	SelectFolder
};

struct FileDialogFilter final
{
	std::string Name;
	std::vector<std::string> Patterns;
};

struct FileDialogSpecification final
{
	FileDialogOperation Operation = FileDialogOperation::OpenFile;
	std::string Title;
	std::filesystem::path InitialDirectory;
	std::string InitialName;
	std::string DefaultExtension;
	std::vector<FileDialogFilter> Filters;
	bool ShowHidden = false;
	bool AddToRecent = false;
	bool RequireExistingPath = true;
	bool ConfirmOverwrite = true;
};

struct FileDialogSelection final
{
	std::vector<std::filesystem::path> Paths;
};

enum class TaskDialogSeverity : uint8
{
	Information,
	Warning,
	Error,
	Shield
};

struct TaskDialogButton final
{
	uint32 ID = 0;
	std::string Text;
	bool IsDefault = false;
};

struct TaskDialogSpecification final
{
	std::string Title;
	std::string Instruction;
	std::string Content;
	std::string ExpandedInformation;
	std::string VerificationText;
	std::vector<TaskDialogButton> Buttons;
	TaskDialogSeverity Severity = TaskDialogSeverity::Information;
	bool AllowCancellation = true;
	bool VerificationChecked = false;
};

struct TaskDialogSelection final
{
	uint32 ButtonID = 0;
	bool VerificationChecked = false;
};

struct DialogColor final
{
	uint8 Red = 255;
	uint8 Green = 255;
	uint8 Blue = 255;
	uint8 Alpha = 255;
	auto operator<=>(const DialogColor &) const = default;
};

struct ColorDialogSpecification final
{
	DialogColor Initial;
	std::vector<DialogColor> CustomColors;
	bool FullOpen = true;
	bool AllowSolidOnly = false;
};

struct FontSelection final
{
	std::string Family;
	float32 PointSize = 12.0f;
	uint16 Weight = 400;
	bool Italic = false;
	bool Underline = false;
	bool Strikeout = false;
	DialogColor Color;
};

struct FontDialogSpecification final
{
	FontSelection Initial;
	float32 MinimumPointSize = 1.0f;
	float32 MaximumPointSize = 256.0f;
	bool ScreenFontsOnly = true;
	bool Effects = true;
};

struct CredentialDialogSpecification final
{
	std::string TargetName;
	std::string Title;
	std::string Message;
	std::string InitialUserName;
	bool AllowSave = false;
	bool SaveChecked = false;
};

struct CredentialSelection final
{
	std::string UserName;
	std::string Domain;
	SecureBuffer Secret;
	bool SaveChecked = false;
};

namespace detail
{
class DialogOperationBase
{
  public:
	virtual ~DialogOperationBase() = default;
	virtual void Wait() noexcept = 0;
};

template <typename Value> class DialogOperation final : public DialogOperationBase
{
  public:
	using Work = std::function<DialogResult<Value>()>;

	static std::shared_ptr<DialogOperation> Start(Work Work, std::function<void()> Wake)
	{
		auto Operation = std::shared_ptr<DialogOperation>(new DialogOperation());
		std::thread(
			[Operation, Work = std::move(Work), Wake = std::move(Wake)]() mutable
			{
				std::optional<DialogResult<Value>> Result;
				std::exception_ptr Failure;
				try
				{
					Result.emplace(Work());
				}
				catch (...)
				{
					Failure = std::current_exception();
				}
				try
				{
					if (Wake)
						Wake();
				}
				catch (...)
				{
					if (Failure == nullptr)
						Failure = std::current_exception();
				}
				if (Failure != nullptr)
					Operation->Fail(std::move(Failure));
				else
					Operation->Complete(std::move(*Result));
			})
			.detach();
		return Operation;
	}

	[[nodiscard]] bool IsReady() const noexcept
	{
		std::scoped_lock Lock(this->Mutex);
		return this->Ready;
	}

	DialogResult<Value> Take()
	{
		std::unique_lock Lock(this->Mutex);
		this->Condition.wait(Lock, [this] { return this->Ready; });
		if (this->Taken)
			throw std::logic_error("Dialog result has already been consumed");
		this->Taken = true;
		if (this->Failure != nullptr)
			std::rethrow_exception(this->Failure);
		return std::move(*this->Result);
	}

	void Wait() noexcept override
	{
		std::unique_lock Lock(this->Mutex);
		this->Condition.wait(Lock, [this] { return this->Ready; });
	}

  private:
	void Complete(DialogResult<Value> Value)
	{
		{
			std::scoped_lock Lock(this->Mutex);
			this->Result.emplace(std::move(Value));
			this->Ready = true;
		}
		this->Condition.notify_all();
	}
	void Fail(std::exception_ptr Exception)
	{
		{
			std::scoped_lock Lock(this->Mutex);
			this->Failure = std::move(Exception);
			this->Ready = true;
		}
		this->Condition.notify_all();
	}

	mutable std::mutex Mutex;
	std::condition_variable Condition;
	std::optional<DialogResult<Value>> Result;
	std::exception_ptr Failure;
	bool Ready = false;
	bool Taken = false;
};
} // namespace detail

template <typename Value> class DialogFuture final
{
  public:
	DialogFuture() = default;
	[[nodiscard]] bool IsValid() const noexcept
	{
		return this->Operation != nullptr;
	}
	[[nodiscard]] bool IsReady() const noexcept
	{
		return this->Operation != nullptr && this->Operation->IsReady();
	}
	DialogResult<Value> Take()
	{
		if (this->Operation == nullptr)
			throw std::logic_error("DialogFuture is empty");
		return this->Operation->Take();
	}

  private:
	friend class Window;
	explicit DialogFuture(std::shared_ptr<detail::DialogOperation<Value>> Operation) : Operation(std::move(Operation))
	{
	}
	std::shared_ptr<detail::DialogOperation<Value>> Operation;
};
} // namespace core
