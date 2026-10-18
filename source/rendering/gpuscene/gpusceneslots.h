#pragma once
#include "gpuscenetypes.h"
#include <vector>
#include <cstring>
#include <stdexcept>

// CPU cache bookkeeping is independently testable; no draw calls or GPU synchronization here.
class GpuSceneSlots
{
    vector<GPUInstanceData> m_Data;
    vector<uint64_t> m_Generations;
    vector<GPUInstanceID> m_Free;
    vector<uint8_t> m_Dirty;
    vector<GPUInstanceID> m_Updates;
    vector<GPUInstanceID> m_Moving;
    uint32_t m_Active = 0;

  public:
    GPUInstanceID Register(const GPUInstanceData& data)
    {
        GPUInstanceID id;
        if (m_Free.empty())
        {
            if (m_Data.size() >= UINT32_MAX)
            {
                throw runtime_error("GPU scene instance ID limit reached");
            }
            id = static_cast<uint32_t>(m_Data.size());
            m_Data.push_back(data);
            m_Generations.push_back(0);
            m_Dirty.push_back(0);
        }
        else
        {
            id = m_Free.back();
            m_Free.pop_back();
            m_Data[id] = data;
        }
        m_Data[id].Flags |= 1u;
        ++m_Generations[id];
        ++m_Active;
        MarkDirty(id);
        return id;
    }

    void Remove(GPUInstanceID id)
    {
        if (!IsActive(id))
        {
            return;
        }
        m_Data[id] = {};
        m_Free.push_back(id);
        --m_Active;
        MarkDirty(id);
    }

    bool IsActive(GPUInstanceID id) const
    {
        return id < m_Data.size() && (m_Data[id].Flags & 1u);
    }

    uint64_t Generation(GPUInstanceID id) const
    {
        return IsActive(id) ? m_Generations[id] : 0;
    }

    void Update(GPUInstanceID id, const GPUInstanceData& data)
    {
        if (!IsActive(id))
        {
            throw runtime_error("Updating a dead GPU scene instance");
        }
        if (memcmp(&m_Data[id], &data, sizeof(data)) == 0)
        {
            return;
        }
        if (memcmp(&m_Data[id].World, &data.World, sizeof(data.World)) != 0)
        {
            m_Moving.push_back(id);
        }
        if (m_Data[id].MeshIndex != data.MeshIndex ||
            m_Data[id].VirtualGeometryAssetIndex != data.VirtualGeometryAssetIndex)
        {
            ++m_Generations[id];
        }
        m_Data[id] = data;
        MarkDirty(id);
    }

    void SettlePreviousTransforms()
    {
        // One follow-up update clears velocity on the first stationary frame after a move.
        for (GPUInstanceID id : m_Moving)
        {
            if (IsActive(id) && memcmp(&m_Data[id].PreviousWorld, &m_Data[id].World, sizeof(m_Data[id].World)) != 0)
            {
                m_Data[id].PreviousWorld = m_Data[id].World;
                MarkDirty(id);
            }
        }
        m_Moving.clear();
    }

    void MarkDirty(GPUInstanceID id)
    {
        if (!m_Dirty.at(id))
        {
            m_Dirty[id] = 1;
            m_Updates.push_back(id);
        }
    }

    void ClearUpdates()
    {
        for (GPUInstanceID id : m_Updates)
        {
            m_Dirty[id] = 0;
        }
        m_Updates.clear();
    }

    uint32_t ActiveCount() const
    {
        return m_Active;
    }

    const vector<GPUInstanceData>& Data() const
    {
        return m_Data;
    }

    const vector<GPUInstanceID>& Updates() const
    {
        return m_Updates;
    }
};
