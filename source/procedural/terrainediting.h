#pragma once

#include "proceduralmath.h"
#include <limits>

namespace Procedural
{
    // CPU のみの編集状態。選択変更と取り消しでは設定だけを再読み込みし、別エンティティに適用しない。
    class TerrainEditState
    {
      public:
        static constexpr uint32_t InvalidTarget = (std::numeric_limits<uint32_t>::max)();

        bool SynchronizeSelection(uint32_t target, const TerrainSettings* appliedSettings);
        void SetSettings(const TerrainSettings& settings);
        bool ShouldApply(double now, bool editingControl) const;
        void MarkApplied(double now);

        const TerrainSettings& GetSettings() const
        {
            return m_Settings;
        }

        bool HasPendingChanges() const
        {
            return m_PendingChanges;
        }

        uint32_t GetTarget() const
        {
            return m_Target;
        }

      private:
        uint32_t m_Target = InvalidTarget;
        TerrainSettings m_Settings;
        TerrainSettings m_AppliedSettings;
        double m_NextApplyTime = 0.0;
        bool m_PendingChanges = false;
    };
} // 名前空間 Procedural の終わり。
