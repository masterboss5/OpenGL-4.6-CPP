#pragma once

#include "src/concepts.h"
#include "src/core/input/InputSystem.h"
#include "src/core/layers/ApplicationLayer.h"
#include "src/core/time/FrameClock.h"
#include "src/core/window/WindowManager.h"
#include "src/types.h"

#include <memory>
#include <vector>

namespace core
{
struct ApplicationSpecification final
{
	WindowSpecification Window;
	bool DeterministicRenderValidation = false;
};

class Application final
{
  private:
	bool Running = false;
	core::WindowManager WindowManager;
	input::InputSystem InputSystem;
	FrameClock FrameClock;
	core::Window *Window = nullptr;
	bool DeterministicRenderValidation = false;
	std::vector<std::unique_ptr<ApplicationLayer>> Layers = {};

  public:
	explicit Application(ApplicationSpecification Specification = {});
	explicit Application(WindowSpecification WindowSpecification);
	~Application();

	void Main();
	void Stop();
	[[nodiscard]] usize GetLayerCount() const;
	[[nodiscard]] core::Window &GetPrimaryWindow() const;
	[[nodiscard]] core::WindowManager &GetWindowManager() noexcept;
	[[nodiscard]] input::InputSystem &GetInputSystem() noexcept;

	template <IsApplicationLayer TLayer> void PushLayer()
	{
		this->Layers.push_back(std::make_unique<TLayer>(this->Window, this->WindowManager.GetDevice(*this->Window)));
	}
};
} // namespace core
