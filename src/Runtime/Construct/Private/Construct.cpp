#include "Construct.h"

#include "JoltPhysics.h"
#include "LogicThreadBase.h"
#include "Registry.h"
#include "WorldBase.h"

void ConstructBase::RegisterView(ConstructViewRef ref)
{
	Views.push_back(ref);
}

void ConstructBase::DeregisterView(void* viewPtr)
{
	for (size_t i = 0; i < Views.size(); ++i)
	{
		if (Views[i].View == viewPtr)
		{
			Views[i] = std::move(Views.back());
			Views.pop_back();
			return;
		}
	}
}

void ConstructBase::CollectViewHandles(std::vector<EntityHandle>& out) const
{
	out.clear();
	for (const auto& v : Views)
		if (v.GetHandleFn) out.push_back(v.GetHandleFn(v.View));
}

Registry* ConstructBase::GetRegistry() const
{
	return OwnerWorld ? OwnerWorld->GetRegistry() : nullptr;
}

void ConstructBase::HydrateAllViews()
{
	for (auto& v : Views)
		v.EnsureHydrated();
}

void ConstructBase::UnbindAllViewContacts(void* self)
{
	JoltPhysics* phys = OwnerWorld->GetPhysics();
	Registry* reg     = OwnerWorld->GetRegistry();
	for (const auto& v : Views)
	{
		if (!v.GetHandleFn) continue;
		phys->UnbindContacts(v.GetHandleFn(v.View), reg, self);
	}
}

void ConstructBase::DeregisterTicks(void* self)
{
	LogicThreadBase* logic = OwnerWorld->GetLogicThread();
	logic->ScalarPrePhysicsBatch.Deregister(self);
	logic->ScalarPostPhysicsBatch.Deregister(self);
	logic->ScalarPhysicsStepBatch.Deregister(self);
	logic->ScalarUpdateBatch.Deregister(self);
}
