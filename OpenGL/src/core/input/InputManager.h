#pragma once
#include<memory>
#include <map>
#include "src/Window.h"
#include "src/Window.h"

namespace core::input
{
	//Describes the state of a key
	enum class KeyState
	{
		UP,
		DOWN,
		REPEAT
	};

	//Describes the state of a mouse button
	enum class MouseButtonState
	{
		UP,
		DOWN
	};
	//TODO add scrolling inputs
	class InputManager final
	{
	private:
		std::unordered_map<int, KeyState> keys;
		std::unordered_map<int, MouseButtonState> mouseButtons;
		float mouseX = 0;
		float mouseY = 0;
		float deltaMouseX = 0;
		float deltaMouseY = 0;
	public:
		InputManager();
		~InputManager();

		InputManager(const InputManager&) = delete;
		InputManager& operator=(const InputManager&) = delete;
		InputManager(InputManager&&) = delete;
		InputManager& operator=(InputManager&&) = delete;

		static InputManager* getInstance();
		void update();
		bool isKeyPressed(int key);
		bool isKeyHeld(int key);
		bool isMouseButtonPressed(int button);
		float getMouseX() const;
		float getMouseY() const;
		float getDeltaMouseX() const;
		float getDeltaMouseY() const;
	};
}