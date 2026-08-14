#include "SelectionSet.h"

#include <algorithm>
#include <stdexcept>

namespace editor::document
{
void SelectionSet::SelectOnly(const util::UUID &Object)
{
	if (!Object.IsValid())
		throw std::invalid_argument("Cannot select an invalid persistent object identity");
	this->Ordered.assign(1, Object);
	this->Membership.clear();
	this->Membership.insert(Object);
	this->Primary = Object;
}

void SelectionSet::Add(const util::UUID &Object)
{
	if (!Object.IsValid())
		throw std::invalid_argument("Cannot select an invalid persistent object identity");
	if (!this->Membership.insert(Object).second)
	{
		this->Primary = Object;
		return;
	}
	this->Ordered.push_back(Object);
	this->Primary = Object;
}

void SelectionSet::Remove(const util::UUID &Object)
{
	if (this->Membership.erase(Object) == 0)
		return;
	std::erase(this->Ordered, Object);
	if (this->Primary == Object)
		this->Primary = this->Ordered.empty() ? util::UUID{} : this->Ordered.back();
}

void SelectionSet::Toggle(const util::UUID &Object)
{
	if (this->Contains(Object))
		this->Remove(Object);
	else
		this->Add(Object);
}

void SelectionSet::Clear() noexcept
{
	this->Ordered.clear();
	this->Membership.clear();
	this->Primary = {};
}

void SelectionSet::Prune(const world::Scene &Scene)
{
	for (usize Index = this->Ordered.size(); Index != 0; --Index)
	{
		if (!Scene.FindObject(this->Ordered[Index - 1]).IsValid())
			this->Remove(this->Ordered[Index - 1]);
	}
}

void SelectionSet::Prune(const instance::InstanceGraph &Graph)
{
	for (auto Iterator = this->Ordered.begin(); Iterator != this->Ordered.end();)
	{
		if (Graph.Contains(*Iterator))
		{
			++Iterator;
			continue;
		}
		this->Membership.erase(*Iterator);
		Iterator = this->Ordered.erase(Iterator);
	}
	this->Primary = this->Ordered.empty() ? util::UUID{} : this->Ordered.back();
}

bool SelectionSet::Contains(const util::UUID &Object) const
{
	return this->Membership.contains(Object);
}

bool SelectionSet::Empty() const noexcept
{
	return this->Ordered.empty();
}

usize SelectionSet::Size() const noexcept
{
	return this->Ordered.size();
}

const util::UUID &SelectionSet::GetPrimary() const noexcept
{
	return this->Primary;
}

const std::vector<util::UUID> &SelectionSet::GetOrdered() const noexcept
{
	return this->Ordered;
}

std::vector<world::ObjectHandle> SelectionSet::Resolve(const world::Scene &Scene) const
{
	std::vector<world::ObjectHandle> Result;
	this->ResolveInto(Scene, Result);
	return Result;
}

void SelectionSet::ResolveInto(const world::Scene &Scene, std::vector<world::ObjectHandle> &Result) const
{
	Result.clear();
	if (Result.capacity() < this->Ordered.size())
		Result.reserve(this->Ordered.size());
	for (const util::UUID &ID : this->Ordered)
	{
		const world::ObjectHandle Object = Scene.FindObject(ID);
		if (Object.IsValid())
			Result.push_back(Object);
	}
}
} // namespace editor::document
