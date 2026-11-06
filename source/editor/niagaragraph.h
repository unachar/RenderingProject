#pragma once
#include "../effects/niagara.h"

namespace ax::NodeEditor
{
    struct EditorContext;
}

namespace NiagaraGraph
{
    // 呼び出し側が編集履歴を記録する。マテリアルのデータモデルとキャンバスを共有する。
    bool Draw(Niagara::Module& module, ax::NodeEditor::EditorContext* context, std::string& diagnostic);
}
