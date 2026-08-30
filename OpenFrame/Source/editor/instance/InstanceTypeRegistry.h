#pragma once

#include "Source/editor/instance/InstanceTypes.h"

#include <shared_mutex>
#include <memory>
#include <unordered_map>

namespace editor::instance
{
struct InstanceTransformCapabilities final
{
	bool Translation = false;
	bool Rotation = false;
	bool Scale = false;

	[[nodiscard]] bool SupportsAnyTransform() const noexcept
	{
		return this->Translation || this->Rotation || this->Scale;
	}
};

struct InstanceTypeDescriptor final
{
	InstanceClassID ClassID;
	string ClassName;
	string DisplayName;
	string Category;
	string IconGlyph;
	glm::vec4 IconColor{1.0F};
	string Description;
	InstanceAvailability Availability = InstanceAvailability::Available;
	bool Creatable = true;
	bool Service = false;
	InstanceTransformCapabilities TransformCapabilities;
	InstancePropertyMap DefaultProperties;
	std::vector<InstancePropertyDescriptor> Properties;
	std::vector<InstanceClassID> ExactParentClasses;
	std::vector<InstanceClassID> AllowedServiceClasses;
};

class InstanceTypeRegistry final
{
  public:
	InstanceTypeRegistry();

	void Register(InstanceTypeDescriptor Descriptor);
	[[nodiscard]] std::shared_ptr<const InstanceTypeDescriptor> Find(const InstanceClassID &ClassID) const;
	[[nodiscard]] std::shared_ptr<const InstanceTypeDescriptor> Find(string_view ClassName) const;
	[[nodiscard]] std::vector<InstanceTypeDescriptor> GetCreatableTypes() const;
	[[nodiscard]] std::vector<InstanceTypeDescriptor> GetTypes() const;

  private:
	mutable std::shared_mutex Mutex;
	std::unordered_map<InstanceClassID, std::shared_ptr<const InstanceTypeDescriptor>> ByID;
	std::unordered_map<string, InstanceClassID> ByName;
};
} // namespace editor::instance
