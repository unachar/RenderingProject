#pragma once
// ============================================================================
// Custom HLSL ノード：ユーザー定義の HLSL「式」の解析 / 型検査 / 評価 / HLSL 出力
//  ・任意の HLSL をそのまま実行しない。許可した文法（数値、入力名、四則、比較、?:、swizzle、floatN()、下の関数）だけを受け付け、
//    それ以外（#、テクスチャ / バッファ、ループ、代入、未許可の関数など）は位置付きのエラーにする
//  ・同じ AST を CPU 評価（プレビュー / ベイク）、HLSL 生成、GPU VM のコンパイル（materialgraphcompiler.cpp）が共有するので、
//    「実行できない環境」は存在しない（式が不正なときだけ、行 / 桁つきのエラーが出る）
// ============================================================================
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace MaterialGraphCustomHlsl
{
    struct Diagnostic
    {
        int Line = 1;   // 1 始まり
        int Column = 1; // 1 始まり
        std::string Message;
        std::string Format() const;
    };

    enum class Op
    {
        Number,    // Number（スカラー）
        Input,     // Index = 入力の番号
        Neg,       // -a
        Not,       // !a
        Add, Sub, Mul, Div,
        Less, Greater, LessEq, GreaterEq, Equal, NotEqual,
        And, Or,
        Ternary,   // Args = {cond, a, b}
        Call,      // Name = 関数名、Args
        Swizzle,   // Mask = "xyz" など、Args = {value}
        Construct  // float2 / float3 / float4。Args = 成分（スカラー / ベクトル）
    };

    struct Node
    {
        Op Kind = Op::Number;
        int Dims = 1; // 型検査後の成分数（1〜4）
        float Value = 0.0f;
        int Index = 0;
        std::string Name;
        std::string Mask;
        int Line = 1;
        int Column = 1;
        std::vector<std::unique_ptr<Node>> Args;
    };

    struct Program
    {
        std::unique_ptr<Node> Root;
        int OutputDims = 1;
        std::string Source;
        std::vector<std::string> InputNames;
        std::vector<int> InputDims;
    };

    struct CompileResult
    {
        std::shared_ptr<const Program> Result;
        std::vector<Diagnostic> Errors;
        bool Ok() const
        {
            return Result != nullptr && Errors.empty();
        }
        std::string FirstError() const;
    };

    // 許可する関数（materialgraphmath.h の単項 / 二項演算と同じ集合 + 数個のベクトル関数）
    bool IsAllowedFunction(const std::string& name);
    const char* AllowedFunctionList();

    // inputDims = 各入力の成分数（接続元から決まる）。outputDims = ノードの出力型（1〜4）。スカラーの式は出力の次元へ広げる
    CompileResult Compile(const std::string& source, const std::vector<std::string>& inputNames,
                          const std::vector<int>& inputDims, int outputDims);

    // CPU 評価。inputs[i] は 4 要素。out は 4 要素（成分数 = program.OutputDims）
    void Evaluate(const Program& program, const std::vector<std::array<float, 4>>& inputs, float* out);

    // HLSL の式（入力は inputExpressions[i]）。出力の次元に合わせて広げた式を返す
    std::string EmitHlsl(const Program& program, const std::vector<std::string>& inputExpressions);

    // 結果を出力の次元に広げる（スカラー → N）。型が合わなければ Compile がエラーにする
} // namespace MaterialGraphCustomHlsl
