#include "InputManager.h"
#include <iostream>
core::input::InputManager::InputManager()
{
    std::cout << "asdfasfasdfdsa";
    glfwSetKeyCallback(Window::windowPtr->window, [](GLFWwindow* glfwWindow, int key, int scancode, int action, int mods) {
        switch (action)
        {
        case GLFW_RELEASE:
            InputManager::getInstance()->keys[key] = core::input::KeyState::UP;
			break;

        case GLFW_PRESS:
			InputManager::getInstance()->keys[key] = core::input::KeyState::DOWN;
            break;

        case GLFW_REPEAT:
            InputManager::getInstance()->keys[key] = core::input::KeyState::REPEAT;
            break;
        }
    });
    
    glfwSetMouseButtonCallback(Window::windowPtr->window, [](GLFWwindow* glfwWindow, int button, int action, int mods) {
        switch (action)
        {
        case GLFW_PRESS:
            InputManager::getInstance()->mouseButtons[button] = core::input::MouseButtonState::DOWN;
            break;

        case GLFW_RELEASE:
            InputManager::getInstance()->mouseButtons[button] = core::input::MouseButtonState::UP;
			break;
        }
	});

    glfwSetCursorPosCallback(Window::windowPtr->window, [](GLFWwindow* glfwWindow, double mouseX, double mouseY) {
        InputManager::getInstance()->deltaMouseX = mouseX - InputManager::getInstance()->mouseX;
        InputManager::getInstance()->deltaMouseY = mouseY - InputManager::getInstance()->mouseY;
        InputManager::getInstance()->mouseX = mouseX;
        InputManager::getInstance()->mouseY = mouseY;
    });
}

core::input::InputManager::~InputManager()
{
	glfwSetKeyCallback(Window::windowPtr->window, nullptr);
	glfwSetMouseButtonCallback(Window::windowPtr->window, nullptr);
	glfwSetCursorPosCallback(Window::windowPtr->window, nullptr);
}

core::input::InputManager* core::input::InputManager::getInstance()
{
    static std::unique_ptr<InputManager> instance = std::make_unique<InputManager>();
    return instance.get();
};

void core::input::InputManager::update()
{
	this->deltaMouseX = 0;
	this->deltaMouseY = 0;
}

bool core::input::InputManager::isKeyPressed(int key)
{
    return this->keys[key] == core::input::KeyState::DOWN or this->keys[key] == core::input::KeyState::REPEAT;
}

bool core::input::InputManager::isKeyHeld(int key)
{
    return this->keys[key] == core::input::KeyState::REPEAT;
}

bool core::input::InputManager::isMouseButtonPressed(int button)
{
    return this->mouseButtons[button] == core::input::MouseButtonState::DOWN;
}

float core::input::InputManager::getMouseX() const
{
    return this->mouseX;
}

float core::input::InputManager::getMouseY() const
{
    return this->mouseY;
}

float core::input::InputManager::getDeltaMouseX() const
{
    return this->deltaMouseX;
}

float core::input::InputManager::getDeltaMouseY() const
{
    return this->deltaMouseY;
}
