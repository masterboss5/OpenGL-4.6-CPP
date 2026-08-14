#pragma once

#include "Source/scene/Scene.h"
#include "Source/editor/instance/InstanceGraph.h"
#include "Source/types.h"
#include "Source/util/UUID.h"

#include <unordered_set>
#include <vector>

namespace editor::document
{
class SelectionSet final
{
  public:
	void SelectOnly(const util::UUID &Object);
	void Add(const util::UUID &Object);
	void Remove(const util::UUID &Object);
	void Toggle(const util::UUID &Object);
	void Clear() noexcept;
	void Prune(const world::Scene &Scene);
	void Prune(const instance::InstanceGraph &Graph);

	[[nodiscard]] bool Contains(const util::UUID &Object) const;
	[[nodiscard]] bool Empty() const noexcept;
	[[nodiscard]] usize Size() const noexcept;
	[[nodiscard]] const util::UUID &GetPrimary() const noexcept;
	[[nodiscard]] const std::vector<util::UUID> &GetOrdered() const noexcept;
	void ResolveInto(const world::Scene &Scene, std::vector<world::ObjectHandle> &Result) const;
	[[nodiscard]] std::vector<world::ObjectHandle> Resolve(const world::Scene &Scene) const;

  private:
	std::vector<util::UUID> Ordered;
	std::unordered_set<util::UUID> Membership;
	util::UUID Primary;
};

// The editor plan names this stable-UUID selection boundary EditorSelection;
// SelectionSet remains the canonical implementation.
using EditorSelection = SelectionSet;
} // namespace editor::document
