#include "terrainediting.h"

namespace Procedural
{
bool TerrainEditState::SynchronizeSelection(uint32_t target, const TerrainSettings* appliedSettings)
{
    const bool selectionChanged = target != m_Target;
    const bool settingsChangedExternally =
        appliedSettings && !TerrainSettingsEqual(*appliedSettings, m_AppliedSettings);
    if (!selectionChanged && !settingsChangedExternally)
    {
        return false;
    }
    m_Target = target;
    m_PendingChanges = false;
    m_NextApplyTime = 0.0;
    if (appliedSettings)
    {
        m_Settings = *appliedSettings;
        m_AppliedSettings = *appliedSettings;
    }
    return true;
}

void TerrainEditState::SetSettings(const TerrainSettings& settings)
{
    m_Settings = settings;
    m_PendingChanges = m_Target != InvalidTarget && !TerrainSettingsEqual(m_Settings, m_AppliedSettings);
}

bool TerrainEditState::ShouldApply(double now, bool editingControl) const
{
    return m_PendingChanges && (!editingControl || now >= m_NextApplyTime);
}

void TerrainEditState::MarkApplied(double now)
{
    m_AppliedSettings = m_Settings;
    m_PendingChanges = false;
    double interval = 0.1;
    if (m_Settings.Resolution > 128)
    {
        interval = 0.25;
    }
    m_NextApplyTime = now + interval;
}
} // namespace Procedural
