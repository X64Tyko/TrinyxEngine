#include "UndoCommand.h"
#include "CacheSlotMeta.h"
#include "TemporalComponentCache.h"

// ============================================================================
// Serialization helpers
// ============================================================================

JsonValue SerializeEntityFields(Registry* reg, Archetype* arch, Chunk* chunk, uint16_t localIndex)
{
    uint32_t temporalFrame = reg->GetTemporalCache()->GetActiveWriteFrame();
    uint32_t volatileFrame = reg->GetVolatileCache()->GetActiveWriteFrame();
    void* fieldArray[MAX_FIELDS_PER_ARCHETYPE];
    arch->BuildFieldArrayTable(chunk, fieldArray, temporalFrame, volatileFrame);

    const auto& cfr = ReflectionRegistry::Get();
    JsonValue obj = JsonValue::Object();

    for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
    {
        const auto* fields = cfr.GetFields(fdesc.componentID);
        if (!fields || fdesc.componentSlotIndex >= fields->size()) continue;
        const char* fieldName = (*fields)[fdesc.componentSlotIndex].Name;
        if (!fieldName) continue;

        void* srcBase = static_cast<uint8_t*>(fieldArray[fdesc.fieldSlotIndex]);
        if (!srcBase) continue;
        void* src = static_cast<uint8_t*>(srcBase) + localIndex * fdesc.fieldSize;

        switch (fdesc.valueType)
        {
            case FieldValueType::Float32:
                obj[fieldName] = JsonValue::Number(*static_cast<const float*>(src));
                break;
            case FieldValueType::Fixed32:
                obj[fieldName] = JsonValue::Number(static_cast<const Fixed32*>(src)->ToFloat());
                break;
            case FieldValueType::Float64:
                obj[fieldName] = JsonValue::Number(*static_cast<const double*>(src));
                break;
            case FieldValueType::Int32:
                obj[fieldName] = JsonValue::Number(static_cast<double>(*static_cast<const int32_t*>(src)));
                break;
            case FieldValueType::Uint32:
                obj[fieldName] = JsonValue::Number(static_cast<double>(*static_cast<const uint32_t*>(src)));
                break;
            default:
                break;
        }
    }
    return obj;
}

void DeserializeEntityFields(Registry* reg, Archetype* arch, Chunk* chunk, uint16_t localIndex, const JsonValue& state)
{
    uint32_t temporalFrame = reg->GetTemporalCache()->GetActiveWriteFrame();
    uint32_t volatileFrame = reg->GetVolatileCache()->GetActiveWriteFrame();
    void* fieldArray[MAX_FIELDS_PER_ARCHETYPE];
    arch->BuildFieldArrayTable(chunk, fieldArray, temporalFrame, volatileFrame);

    const auto& cfr = ReflectionRegistry::Get();

    for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
    {
        const auto* fields = cfr.GetFields(fdesc.componentID);
        if (!fields || fdesc.componentSlotIndex >= fields->size()) continue;
        const char* fieldName = (*fields)[fdesc.componentSlotIndex].Name;
        if (!fieldName) continue;

        const JsonValue* val = state.Find(fieldName);
        if (!val) continue;

        void* dstBase = static_cast<uint8_t*>(fieldArray[fdesc.fieldSlotIndex]);
        if (!dstBase) continue;
        void* dst = static_cast<uint8_t*>(dstBase) + localIndex * fdesc.fieldSize;

        switch (fdesc.valueType)
        {
            case FieldValueType::Float32:
                *static_cast<float*>(dst) = static_cast<float>(val->AsNumber());
                break;
            case FieldValueType::Fixed32:
                *static_cast<Fixed32*>(dst) = Fixed32::FromFloat(val->AsFloat());
                break;
            case FieldValueType::Float64:
                *static_cast<double*>(dst) = val->AsNumber();
                break;
            case FieldValueType::Int32:
                *static_cast<int32_t*>(dst) = val->AsInt();
                break;
            case FieldValueType::Uint32:
                *static_cast<uint32_t*>(dst) = static_cast<uint32_t>(val->AsInt());
                break;
            default:
                break;
        }
    }
}

void MarkEntityDirty(Registry* reg, Archetype* arch, Chunk* chunk, uint16_t localIndex)
{
    Archetype::FieldKey flagKey{
        CacheSlotMeta<>::StaticTypeID(),
        ReflectionRegistry::Get().GetCacheSlotIndex(CacheSlotMeta<>::StaticTypeID()),
        0
    };
    const auto* flagDesc = arch->ArchetypeFieldLayout.find(flagKey);
    if (!flagDesc) return;

    auto* base = static_cast<uint8_t*>(chunk->GetFieldPtr(flagDesc->fieldSlotIndex));
    if (!base) return;

    auto* cache = reg->GetTemporalCache();
    auto* flags = reinterpret_cast<int32_t*>(cache->GetWriteFramePtr(base));
    flags[localIndex] |= static_cast<int32_t>(TemporalFlagBits::Dirty);
}

void* GetFieldPtr(Archetype* arch, Chunk* chunk, const char* fieldName, uint16_t localIndex, Registry* reg)
{
    uint32_t temporalFrame = reg->GetTemporalCache()->GetActiveWriteFrame();
    uint32_t volatileFrame = reg->GetVolatileCache()->GetActiveWriteFrame();
    void* fieldArray[MAX_FIELDS_PER_ARCHETYPE];
    arch->BuildFieldArrayTable(chunk, fieldArray, temporalFrame, volatileFrame);

    const auto& cfr = ReflectionRegistry::Get();
    for (const auto& [fkey, fdesc] : arch->ArchetypeFieldLayout)
    {
        const auto* fields = cfr.GetFields(fdesc.componentID);
        if (!fields || fdesc.componentSlotIndex >= fields->size()) continue;
        const char* name = (*fields)[fdesc.componentSlotIndex].Name;
        if (name && std::strcmp(name, fieldName) == 0)
        {
            void* base = fieldArray[fdesc.fieldSlotIndex];
            if (!base) return nullptr;
            return static_cast<uint8_t*>(base) + localIndex * fdesc.fieldSize;
        }
    }
    return nullptr;
}

// ============================================================================
// EntityTransformCommand
// ============================================================================

void EntityTransformCommand::Execute()
{
    DeserializeEntityFields(m_Reg, m_Arch, m_Chunk, m_LocalIndex, m_After);
    MarkEntityDirty(m_Reg, m_Arch, m_Chunk, m_LocalIndex);
}

void EntityTransformCommand::Undo()
{
    DeserializeEntityFields(m_Reg, m_Arch, m_Chunk, m_LocalIndex, m_Before);
    MarkEntityDirty(m_Reg, m_Arch, m_Chunk, m_LocalIndex);
}

bool EntityTransformCommand::MergeWith(const UndoCommand& other)
{
    const auto* rhs = dynamic_cast<const EntityTransformCommand*>(&other);
    if (!rhs) return false;
    if (m_Arch == rhs->m_Arch && m_Chunk == rhs->m_Chunk && m_LocalIndex == rhs->m_LocalIndex)
    {
        m_After = rhs->m_After;
        return true;
    }
    return false;
}

// ============================================================================
// ComponentFieldChangeCommand
// ============================================================================

void ComponentFieldChangeCommand::Execute()
{
    void* ptr = GetFieldPtr(m_Arch, m_Chunk, m_FieldName.c_str(), m_LocalIndex, m_Reg);
    if (ptr)
    {
        std::memcpy(ptr, m_NewValue.data(), m_FieldSize);
        MarkEntityDirty(m_Reg, m_Arch, m_Chunk, m_LocalIndex);
    }
}

void ComponentFieldChangeCommand::Undo()
{
    void* ptr = GetFieldPtr(m_Arch, m_Chunk, m_FieldName.c_str(), m_LocalIndex, m_Reg);
    if (ptr)
    {
        std::memcpy(ptr, m_OldValue.data(), m_FieldSize);
        MarkEntityDirty(m_Reg, m_Arch, m_Chunk, m_LocalIndex);
    }
}

bool ComponentFieldChangeCommand::MergeWith(const UndoCommand& other)
{
    const auto* rhs = dynamic_cast<const ComponentFieldChangeCommand*>(&other);
    if (!rhs) return false;
    if (m_Arch == rhs->m_Arch && m_Chunk == rhs->m_Chunk &&
        m_LocalIndex == rhs->m_LocalIndex && m_FieldName == rhs->m_FieldName)
    {
        m_NewValue = rhs->m_NewValue;
        return true;
    }
    return false;
}
