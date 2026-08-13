#include "PropertyEditCommand.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace editor::commands
{
EditorCommandPtr PropertyEditCommand::Create(world::Scene &Scene, const world::ObjectHandle Object, const uint32 ComponentType,
											 reflection::PropertyDescriptor Property, reflection::PropertyValue After,
											 resource::AssetManager *Assets)
{
	return PropertyEditCommand::Create(Scene, std::span<const world::ObjectHandle>(&Object, 1), ComponentType, std::move(Property),
									   std::move(After), Assets);
}

EditorCommandPtr PropertyEditCommand::Create(world::Scene &Scene, const std::span<const world::ObjectHandle> Objects,
											 const uint32 ComponentType, reflection::PropertyDescriptor Property,
											 reflection::PropertyValue After, resource::AssetManager *Assets)
{
	if (Objects.empty())
		throw std::invalid_argument("A property edit command requires at least one target");
	if (!Property.Write || reflection::HasFlag(Property.Flags, reflection::PropertyFlags::ReadOnly))
		throw std::invalid_argument("A property edit command requires a writable reflected property");

	std::vector<world::ObjectHandle> UniqueObjects;
	std::vector<reflection::PropertyValue> Before;
	UniqueObjects.reserve(Objects.size());
	Before.reserve(Objects.size());
	for (const world::ObjectHandle Object : Objects)
	{
		if (!Object.IsValid() || !Scene.Contains(Object))
			throw std::invalid_argument("A property edit command requires live object handles");
	}
	{
		auto Access = Scene.Read();
		for (const world::ObjectHandle Object : Objects)
		{
			if (std::ranges::find(UniqueObjects, Object) != UniqueObjects.end())
				continue;
			const components::CObjectComponent &Component = Access.ResolveComponent(Object, ComponentType);
			UniqueObjects.push_back(Object);
			Before.push_back(Property.Read(&Component));
		}
	}
	return EditorCommandPtr(new PropertyEditCommand(Scene, std::move(UniqueObjects), ComponentType, std::move(Property), std::move(Before),
													std::move(After), Assets));
}

PropertyEditCommand::PropertyEditCommand(world::Scene &Scene, std::vector<world::ObjectHandle> Objects, const uint32 ComponentType,
										 reflection::PropertyDescriptor Property, std::vector<reflection::PropertyValue> Before,
										 reflection::PropertyValue After, resource::AssetManager *Assets)
	: Scene(&Scene), Objects(std::move(Objects)), ComponentType(ComponentType), Property(std::move(Property)), Before(std::move(Before)),
	  After(std::move(After)), Assets(Assets), Name("Set " + this->Property.DisplayName)
{
}

string_view PropertyEditCommand::GetName() const noexcept
{
	return this->Name;
}

void PropertyEditCommand::Execute()
{
	this->ApplyUniform(this->After);
}

void PropertyEditCommand::Undo()
{
	this->ApplyValues(this->Before);
}

bool PropertyEditCommand::TryMerge(const EditorCommand &Other)
{
	const auto *Typed = dynamic_cast<const PropertyEditCommand *>(&Other);
	if (Typed == nullptr || this->Scene != Typed->Scene || this->Objects != Typed->Objects || this->ComponentType != Typed->ComponentType ||
		this->Property.Name != Typed->Property.Name)
	{
		return false;
	}
	this->After = Typed->After;
	return true;
}

void PropertyEditCommand::ApplyUniform(const reflection::PropertyValue &Value)
{
	std::vector<reflection::PropertyValue> Values(this->Objects.size(), Value);
	this->ApplyValues(Values);
}

void PropertyEditCommand::ApplyValues(const std::span<const reflection::PropertyValue> Values)
{
	if (Values.size() != this->Objects.size())
		throw std::logic_error("Property edit value count does not match its target count");
	for (const world::ObjectHandle Object : this->Objects)
	{
		if (!this->Scene->Contains(Object))
			throw std::out_of_range("Property edit target no longer exists in its scene");
	}
	auto Access = this->Scene->Write();
	std::vector<reflection::PropertyValue> RollbackValues;
	RollbackValues.reserve(this->Objects.size());
	for (const world::ObjectHandle Object : this->Objects)
	{
		const components::CObjectComponent &Component = Access.ResolveComponent(Object, this->ComponentType);
		RollbackValues.push_back(this->Property.Read(&Component));
	}

	usize Applied = 0;
	try
	{
		for (; Applied < this->Objects.size(); ++Applied)
		{
			components::CObjectComponent &Component = Access.ResolveComponent(this->Objects[Applied], this->ComponentType);
			this->Property.Write(&Component, Values[Applied], {.Scene = this->Scene, .Assets = this->Assets});
		}
	}
	catch (...)
	{
		const std::exception_ptr Failure = std::current_exception();
		try
		{
			while (Applied != 0)
			{
				--Applied;
				components::CObjectComponent &Component = Access.ResolveComponent(this->Objects[Applied], this->ComponentType);
				this->Property.Write(&Component, RollbackValues[Applied], {.Scene = this->Scene, .Assets = this->Assets});
			}
		}
		catch (...)
		{
			throw std::runtime_error("Property edit failed and its atomic rollback also failed");
		}
		std::rethrow_exception(Failure);
	}
}
} // namespace editor::commands
