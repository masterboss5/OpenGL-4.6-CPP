#pragma once
#include "src/util/UUID.h"

namespace resource
{
	class Asset
	{
	private:
		util::UUID uuid;
		int referenceCount = 0;
	public:
		explicit Asset(util::UUID uuid)
			: uuid(uuid)
		{
		}

		virtual ~Asset() = default;

		const util::UUID& getUUID() const
		{
			return this->uuid;
		}

		void incrementReferenceCount()
		{
			this->referenceCount++;
		}

		void decrementReferenceCount()
		{
			this->referenceCount--;
		}

		int getReferenceCount() const
		{
			return this->referenceCount;
		}
	};
}