#include "framework.h"

class Application;

class Window
{
public:
	int width, height;

	Window(Application *);
	GLFWwindow* wndptr;

	void processInput();

private:
	Application* app;

	bool firstMouse = true;
	float lastX, lastY;
};