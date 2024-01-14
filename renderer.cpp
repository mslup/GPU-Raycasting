#include "framework.h"

#include <execution>
#include <algorithm>
#include <glm/gtx/string_cast.hpp>

Renderer::Renderer(int width, int height, Application *parent)
{
	app = parent;
	imageData = nullptr;

	scene.create();

	gpuErrchk(cudaMalloc(&cudaImage, width * height * sizeof(unsigned int)));

	camera = new Camera(width, height);
	resize(width, height);
}

Renderer::~Renderer()
{
	if (imageData != nullptr)
		delete[] imageData;

	gpuErrchk(cudaFree(cudaImage));
}

void Renderer::resize(int width, int height)
{
	this->width = width;
	this->height = height;

	if (imageData != nullptr)
	{
		delete[] imageData;
		gpuErrchk(cudaFree(cudaImage));
	}

	imageData = new unsigned int[width * height];
	gpuErrchk(cudaMalloc(&cudaImage, width * height * sizeof(unsigned int)));

	camera->onResize(width, height);
}

void Renderer::update(float deltaTime)
{
	static float time = glfwGetTime();

	scene.lightPositions[0] = glm::vec3(
		//(glfwGetTime() - time), 0.0f, 0.0f
		2.5f * glm::sin(glfwGetTime()),
		2.5f * glm::cos(glfwGetTime()),
		1.5f * glm::sin(glfwGetTime())
	);
}

void Renderer::render()
{
	// todo: cache the ray directions
	camera->calculateRayDirections();

	// todo: make two functions renderCPU and renderGPU
	if (app->solutionMode == app->CPU)
	{
		// todo: don't resize each time
		std::vector<GLuint> horizontalIter;
		std::vector<GLuint> verticalIter;

		horizontalIter.resize(width);
		verticalIter.resize(height);
		for (uint32_t i = 0; i < width; i++)
			horizontalIter[i] = i;
		for (uint32_t i = 0; i < height; i++)
			verticalIter[i] = i;

		std::for_each(std::execution::par, verticalIter.begin(), verticalIter.end(),
			[this, horizontalIter](uint32_t i)
			{
				std::for_each(std::execution::par, horizontalIter.begin(), horizontalIter.end(),
				[this, i](uint32_t j)
					{
						imageData[i * width + j] = toRGBA(rayGen(i, j));
					});
			});
	}
	else if (app->solutionMode == app->GPU)
	{
		const int max_threads = 1024;
		int blocks_per_grid = (width * height + max_threads - 1) / max_threads;

		int pixelsCount = width * height;
		int size = pixelsCount * sizeof(unsigned int);
		
		callKernels(blocks_per_grid, max_threads, cudaImage, pixelsCount,
			width, height);

		gpuErrchk(cudaMemcpy(imageData, cudaImage, size, cudaMemcpyDeviceToHost));
	}
}

GLuint* Renderer::getImage()
{
	return imageData;
}

GLuint Renderer::toRGBA(glm::vec4& color)
{
	unsigned char r = color.r * 255.0f;
	unsigned char g = color.g * 255.0f;
	unsigned char b = color.b * 255.0f;
	unsigned char a = color.a * 255.0f;

	return (r << 24) | (g << 16) | (b << 8) | a;
}

glm::vec4 Renderer::rayGen(int i, int j)
{
	Ray ray;

	ray.origin = camera->getRayOrigin(); //camera->getOrthographicRayOrigins()[i * width + j];
	ray.direction = camera->getRayDirections()[i * width + j];

	HitPayload payload = traceRayFromPixel(ray);
	int idx = payload.objectIndex;

	// no sphere detected
	if (payload.hitDistance < 0)
		return glm::vec4(skyColor, 1.0f);

	// light source hit
	if (payload.hitDistance == 0)
		return glm::vec4(scene.lightColors[idx], 1.0f);

	glm::vec4 color = glm::vec4(kAmbient * ambientColor * scene.sphereAlbedos[idx], 1.0f);

	// cast rays from hitpoint to light sources
	for (int lightIdx = 0; lightIdx < scene.lightCount; lightIdx++)
	{
		Ray rayToLight;

		// cast ray a bit away from the sphere so that the ray doesn't hit it
		rayToLight.origin = payload.hitPoint + payload.normal * 1e-4f;
		// todo: double calculated length
		float distanceToLight = glm::length(scene.lightPositions[lightIdx] - payload.hitPoint);
		rayToLight.direction = glm::normalize(scene.lightPositions[lightIdx] - payload.hitPoint);

		HitPayload payloadToLight = traceRayFromHitpoint(rayToLight, distanceToLight);

		// no sphere hit on path to light
		if (payloadToLight.hitDistance < 0)
			color += phong(payload, lightIdx);
	}

	return glm::clamp(color, 0.0f, 1.0f);
}

glm::vec4 Renderer::phong(HitPayload payload, int lightIndex)
{
	glm::vec3 lightDir = glm::normalize(scene.lightPositions[lightIndex] - payload.hitPoint);
	glm::vec3 lightColor = scene.lightColors[lightIndex];
	float cosNL = glm::max(0.0f, glm::dot(lightDir, payload.normal));
	glm::vec3 reflectionVector = glm::reflect(-lightDir, payload.normal);
	glm::vec3 eyeVector = glm::normalize(camera->position - payload.hitPoint);
	float cosVR = glm::max(0.0f, glm::dot(reflectionVector, eyeVector));

	glm::vec3 color =
		kDiffuse * cosNL * lightColor +
		kSpecular * glm::pow(cosVR, kShininess) * lightColor;

	color *= scene.sphereAlbedos[payload.objectIndex];

	return glm::vec4(color, 1.0f);
}

// todo: merge two traceray functions
Renderer::HitPayload Renderer::traceRayFromPixel(const Ray& ray)
{
	int hitSphereIndex = -1;
	int hitLightIndex = -1;
	float hitDistance = FLT_MAX;

	for (int k = 0; k < scene.sphereCount; k++)
	{
		glm::vec3 origin = ray.origin - scene.spherePositions[k];
		glm::vec3 direction = ray.direction;

		float radius = scene.sphereRadii[k];

		float a = glm::dot(direction, direction);
		float b = 2.0f * glm::dot(origin, direction);
		float c = glm::dot(origin, origin)
			- radius * radius;

		float delta = b * b - 4.0f * a * c;
		if (delta < 0)
			continue;

		float t = (-b - glm::sqrt(delta)) / (2.0f * a);

		if (t > 0 && t < hitDistance)
		{
			hitDistance = t;
			hitSphereIndex = k;
		}
	}

	for (int k = 0; k < scene.lightCount; k++)
	{
		glm::vec3 origin = ray.origin - scene.lightPositions[k];
		glm::vec3 direction = ray.direction;

		float a = glm::dot(direction, direction);
		float b = 2.0f * glm::dot(origin, direction);
		float c = glm::dot(origin, origin)
			- 0.1f * 0.1f;

		float delta = b * b - 4.0f * a * c;
		if (delta < 0)
			continue;

		float t = (-b - glm::sqrt(delta)) / (2.0f * a);

		if (t > 0 && t < hitDistance)
		{
			hitDistance = t;
			hitSphereIndex = -2;
			hitLightIndex = k;
		}
	}

	if (hitSphereIndex == -1)
		return miss(ray);

	if (hitSphereIndex == -2)
		return lightHit(ray, hitLightIndex);

	return closestHit(ray, hitSphereIndex, hitDistance);
}

Renderer::HitPayload Renderer::traceRayFromHitpoint(const Ray& ray, float diff)
{
	int hitSphereIndex = -1;
	float hitDistance = FLT_MAX;

	for (int k = 0; k < scene.sphereCount; k++)
	{
		glm::vec3 origin = ray.origin - scene.spherePositions[k];
		glm::vec3 direction = ray.direction;

		float radius = scene.sphereRadii[k];

		float a = glm::dot(direction, direction);
		float b = 2.0f * glm::dot(origin, direction);
		float c = glm::dot(origin, origin)
			- radius * radius;

		float delta = b * b - 4.0f * a * c;
		if (delta < 0)
			continue;

		float t = (-b - glm::sqrt(delta)) / (2.0f * a);

		if (t > 0 && t < diff && t < hitDistance)
		{
			hitDistance = t;
			hitSphereIndex = k;
		}
	}

	if (hitSphereIndex == -1)
		return miss(ray);

	return closestHit(ray, hitSphereIndex, hitDistance);
}

Renderer::HitPayload Renderer::miss(const Ray& ray)
{
	HitPayload payload;
	payload.hitDistance = -1.0f;
	return payload;
}

Renderer::HitPayload Renderer::lightHit(const Ray& ray, int lightIndex)
{
	HitPayload payload;
	payload.hitDistance = 0.0f;
	payload.objectIndex = lightIndex;
	return payload;
}

Renderer::HitPayload Renderer::closestHit(const Ray& ray, int sphereIndex, float hitDistance)
{
	HitPayload payload;

	payload.hitDistance = hitDistance;
	payload.objectIndex = sphereIndex;

	glm::vec3 sphereCenter = scene.spherePositions[sphereIndex];

	payload.hitPoint = ray.origin + ray.direction * hitDistance;
	payload.normal = glm::normalize(payload.hitPoint - sphereCenter);

	return payload;
}

void Renderer::processKeyboard(int key, float deltaTime)
{
	camera->onUpdate(key, deltaTime);
}

void Renderer::processMouse(glm::vec2 offset, float deltaTime)
{
	camera->onMouseUpdate(offset, deltaTime);
}