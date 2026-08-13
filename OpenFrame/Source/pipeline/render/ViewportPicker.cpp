#include "ViewportPicker.h"

#include "Source/pipeline/device/Device.h"

#include <stdexcept>

namespace pipeline::render
{
ViewportPicker::ViewportPicker(pipeline::device::Device &Device) : Device(Device)
{
	(void)this->Device->RequireCurrentContext();
	try
	{
		for (usize Index = 0; Index < this->Slots.size(); ++Index)
		{
			ReadbackSlot &Slot = this->Slots[Index];
			glCreateBuffers(1, &Slot.Buffer);
			if (Slot.Buffer == 0)
				throw pipeline::device::DeviceError("OpenGL could not allocate a viewport-picking readback buffer");
			glNamedBufferStorage(Slot.Buffer, sizeof(PickID), nullptr,
								 GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_CLIENT_STORAGE_BIT);
			this->Device->CheckErrors("Viewport picker readback-buffer storage");
			Slot.Mapped = static_cast<PickID *>(
				glMapNamedBufferRange(Slot.Buffer, 0, sizeof(PickID), GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT));
			if (Slot.Mapped == nullptr)
				throw std::runtime_error("Viewport picker could not persistently map a readback buffer");
			const string Label = "ViewportPickReadback-" + std::to_string(Index);
			glObjectLabel(GL_BUFFER, Slot.Buffer, static_cast<GLsizei>(Label.size()), Label.c_str());
		}
		this->Device->CheckErrors("Viewport picker creation");
	}
	catch (...)
	{
		for (ReadbackSlot &Slot : this->Slots)
		{
			if (Slot.Buffer == 0)
				continue;
			if (Slot.Mapped != nullptr)
				glUnmapNamedBuffer(Slot.Buffer);
			glDeleteBuffers(1, &Slot.Buffer);
			Slot = {};
		}
		throw;
	}
}

ViewportPicker::~ViewportPicker()
{
	this->CancelAll();
	if (!this->Device)
		return;
	const bool CanUnmap = this->Device->CanIssueCommands();
	for (ReadbackSlot &Slot : this->Slots)
	{
		if (Slot.Buffer == 0)
			continue;
		if (Slot.Mapped != nullptr && CanUnmap)
			glUnmapNamedBuffer(Slot.Buffer);
		this->Device->RetireGPUObject(pipeline::device::GPUObjectType::Buffer, Slot.Buffer);
		Slot.Buffer = 0;
		Slot.Mapped = nullptr;
	}
}

bool ViewportPicker::TryRequest(const GLuint ObjectIDTexture, const pipeline::graph::Extent2D Extent, const uint32 X, const uint32 Y,
								const PickRequestID Request, const uint64 SourceFrame)
{
	(void)this->Device->RequireCurrentContext();
	if (ObjectIDTexture == 0 || !Extent.IsValid())
		throw std::invalid_argument("Viewport picking requires a valid object-ID texture and extent");
	if (X >= Extent.Width || Y >= Extent.Height)
		throw std::out_of_range("Viewport pick coordinate is outside the object-ID texture");
	if (Request == 0)
		throw std::invalid_argument("Viewport pick request identity must be non-zero");

	ReadbackSlot &Slot = this->Slots[this->NextSlot];
	if (Slot.Pending)
		return false;

	glBindBuffer(GL_PIXEL_PACK_BUFFER, Slot.Buffer);
	glGetTextureSubImage(ObjectIDTexture, 0, static_cast<GLint>(X), static_cast<GLint>(Y), 0, 1, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_INT,
						 sizeof(PickID), nullptr);
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	Slot.Fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	if (Slot.Fence == nullptr)
	{
		glFinish();
		throw std::runtime_error("Viewport picker could not create a readback fence");
	}
	Slot.Request = Request;
	Slot.SourceFrame = SourceFrame;
	Slot.Pending = true;
	++this->PendingCount;
	this->NextSlot = (this->NextSlot + 1) % this->Slots.size();
	this->Device->CheckErrors("Viewport pick request");
	return true;
}

void ViewportPicker::PollInto(std::vector<PickReadbackResult> &Results)
{
	(void)this->Device->RequireCurrentContext();
	Results.clear();
	Results.reserve(this->PendingCount);
	for (ReadbackSlot &Slot : this->Slots)
	{
		std::optional<PickReadbackResult> Result = this->PollSlot(Slot);
		if (Result.has_value())
			Results.push_back(*Result);
	}
}

std::vector<PickReadbackResult> ViewportPicker::Poll()
{
	std::vector<PickReadbackResult> Results;
	this->PollInto(Results);
	return Results;
}

void ViewportPicker::CancelAll() noexcept
{
	if (!this->Device || !this->Device->CanIssueCommands())
		return;
	for (ReadbackSlot &Slot : this->Slots)
		this->ResetSlot(Slot);
	this->PendingCount = 0;
}

usize ViewportPicker::GetPendingCount() const noexcept
{
	return this->PendingCount;
}

usize ViewportPicker::GetAvailableRequestCount() const noexcept
{
	usize Result = 0;
	for (usize Offset = 0; Offset < this->Slots.size(); ++Offset)
	{
		const usize Index = (this->NextSlot + Offset) % this->Slots.size();
		if (this->Slots[Index].Pending)
			break;
		++Result;
	}
	return Result;
}

std::optional<PickReadbackResult> ViewportPicker::PollSlot(ReadbackSlot &Slot)
{
	if (!Slot.Pending)
		return std::nullopt;
	const GLenum Status = glClientWaitSync(Slot.Fence, 0, 0);
	if (Status == GL_TIMEOUT_EXPIRED)
		return std::nullopt;
	if (Status == GL_WAIT_FAILED)
		throw std::runtime_error("Viewport pick readback fence wait failed");
	if (Status != GL_ALREADY_SIGNALED && Status != GL_CONDITION_SATISFIED)
		throw std::runtime_error("Viewport pick readback returned an unknown fence status");

	const PickReadbackResult Result{.Request = Slot.Request, .Pick = *Slot.Mapped, .SourceFrame = Slot.SourceFrame};
	this->ResetSlot(Slot);
	--this->PendingCount;
	return Result;
}

void ViewportPicker::ResetSlot(ReadbackSlot &Slot) noexcept
{
	if (Slot.Fence != nullptr)
		glDeleteSync(Slot.Fence);
	Slot.Fence = nullptr;
	Slot.Request = 0;
	Slot.SourceFrame = 0;
	Slot.Pending = false;
}
} // namespace pipeline::render
