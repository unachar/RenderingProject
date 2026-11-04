#include "pch.h"
#include "materialgraphcustomhlsl.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <sstream>

namespace MaterialGraphCustomHlsl
{
    namespace
    {
        struct FunctionInfo
        {
            const char* Name;
            int Arity;
            int Kind; // 0 = 成分ごと（1 引数）、1 = 成分ごと（2 引数）、2 = 3 引数、3 = ベクトル関数
        };
        const FunctionInfo kFunctions[] = {
            {"abs", 1, 0},      {"ceil", 1, 0},   {"floor", 1, 0},      {"frac", 1, 0},       {"round", 1, 0},
            {"trunc", 1, 0},    {"sign", 1, 0},   {"saturate", 1, 0},   {"sqrt", 1, 0},       {"sin", 1, 0},
            {"cos", 1, 0},      {"tan", 1, 0},    {"asin", 1, 0},       {"acos", 1, 0},       {"atan", 1, 0},
            {"exp", 1, 0},      {"log2", 1, 0},   {"pow", 2, 1},        {"min", 2, 1},        {"max", 2, 1},
            {"fmod", 2, 1},     {"step", 2, 1},   {"atan2", 2, 1},      {"lerp", 3, 2},       {"clamp", 3, 2},
            {"smoothstep", 3, 2}, {"dot", 2, 3},  {"cross", 2, 3},      {"length", 1, 3},     {"distance", 2, 3},
            {"normalize", 1, 3}};

        const FunctionInfo* FindFunction(const std::string& name)
        {
            for (const auto& f : kFunctions)
            {
                if (name == f.Name)
                {
                    return &f;
                }
            }
            return nullptr;
        }

        // ------------------------------------------------------------------ 字句
        enum class Tok { End, Number, Ident, Punct };
        struct Token
        {
            Tok Kind = Tok::End;
            std::string Text;
            float Number = 0.0f;
            int Line = 1;
            int Column = 1;
        };

        class Lexer
        {
          public:
            explicit Lexer(const std::string& s) : m_S(s) {}
            bool Next(Token& t, Diagnostic& error)
            {
                SkipSpace();
                t = {};
                t.Line = m_Line;
                t.Column = m_Col;
                if (m_I >= m_S.size())
                {
                    t.Kind = Tok::End;
                    return true;
                }
                const char c = m_S[m_I];
                if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && m_I + 1 < m_S.size() && std::isdigit(static_cast<unsigned char>(m_S[m_I + 1]))))
                {
                    size_t j = m_I;
                    while (j < m_S.size() && (std::isdigit(static_cast<unsigned char>(m_S[j])) || m_S[j] == '.'))
                    {
                        ++j;
                    }
                    if (j < m_S.size() && (m_S[j] == 'e' || m_S[j] == 'E'))
                    {
                        size_t k = j + 1;
                        if (k < m_S.size() && (m_S[k] == '+' || m_S[k] == '-'))
                        {
                            ++k;
                        }
                        if (k < m_S.size() && std::isdigit(static_cast<unsigned char>(m_S[k])))
                        {
                            while (k < m_S.size() && std::isdigit(static_cast<unsigned char>(m_S[k])))
                            {
                                ++k;
                            }
                            j = k;
                        }
                    }
                    const std::string text = m_S.substr(m_I, j - m_I);
                    char* end = nullptr;
                    t.Number = std::strtof(text.c_str(), &end);
                    if (!end || *end != '\0' || !std::isfinite(t.Number))
                    {
                        error = {t.Line, t.Column, "数値が不正です: " + text};
                        return false;
                    }
                    if (j < m_S.size() && (m_S[j] == 'f' || m_S[j] == 'F'))
                    {
                        ++j;
                    }
                    Advance(j - m_I);
                    t.Kind = Tok::Number;
                    t.Text = text;
                    return true;
                }
                if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
                {
                    size_t j = m_I;
                    while (j < m_S.size() && (std::isalnum(static_cast<unsigned char>(m_S[j])) || m_S[j] == '_'))
                    {
                        ++j;
                    }
                    t.Kind = Tok::Ident;
                    t.Text = m_S.substr(m_I, j - m_I);
                    Advance(j - m_I);
                    return true;
                }
                static const char* two[] = {"<=", ">=", "==", "!=", "&&", "||"};
                for (const char* p : two)
                {
                    if (m_S.compare(m_I, 2, p) == 0)
                    {
                        t.Kind = Tok::Punct;
                        t.Text = p;
                        Advance(2);
                        return true;
                    }
                }
                static const std::string single = "+-*/()<>,.?:!;";
                if (single.find(c) != std::string::npos)
                {
                    t.Kind = Tok::Punct;
                    t.Text = std::string(1, c);
                    Advance(1);
                    return true;
                }
                error = {t.Line, t.Column, std::string("使えない文字です: '") + c + "'（許可: 数値、入力名、+ - * / 比較、?:、swizzle、floatN()、許可した関数）"};
                return false;
            }

          private:
            void SkipSpace()
            {
                while (m_I < m_S.size())
                {
                    const char c = m_S[m_I];
                    if (c == '/' && m_I + 1 < m_S.size() && m_S[m_I + 1] == '/')
                    {
                        while (m_I < m_S.size() && m_S[m_I] != '\n')
                        {
                            Advance(1);
                        }
                    }
                    else if (std::isspace(static_cast<unsigned char>(c)))
                    {
                        Advance(1);
                    }
                    else
                    {
                        break;
                    }
                }
            }
            void Advance(size_t n)
            {
                for (size_t k = 0; k < n && m_I < m_S.size(); ++k, ++m_I)
                {
                    if (m_S[m_I] == '\n')
                    {
                        ++m_Line;
                        m_Col = 1;
                    }
                    else
                    {
                        ++m_Col;
                    }
                }
            }
            const std::string& m_S;
            size_t m_I = 0;
            int m_Line = 1;
            int m_Col = 1;
        };

        using NodePtr = std::unique_ptr<Node>;

        class Parser
        {
          public:
            Parser(const std::string& source, const std::vector<std::string>& names, const std::vector<int>& dims, std::vector<Diagnostic>& errors)
                : m_Lexer(source), m_Names(names), m_Dims(dims), m_Errors(errors)
            {
                Advance();
            }

            NodePtr ParseProgram()
            {
                if (m_Cur.Kind == Tok::Ident && m_Cur.Text == "return")
                {
                    Advance();
                }
                NodePtr expr = ParseTernary();
                if (m_Failed)
                {
                    return nullptr;
                }
                if (m_Cur.Kind == Tok::Punct && m_Cur.Text == ";")
                {
                    Advance();
                }
                if (m_Cur.Kind != Tok::End)
                {
                    Fail(m_Cur, "式の後ろに余分な文字があります（1 つの式だけ書けます。代入・宣言・ループは使えません）: " + m_Cur.Text);
                    return nullptr;
                }
                return expr;
            }

          private:
            void Advance()
            {
                Diagnostic error;
                if (!m_Lexer.Next(m_Cur, error))
                {
                    m_Errors.push_back(error);
                    m_Failed = true;
                    m_Cur = {};
                }
            }
            void Fail(const Token& at, const std::string& message)
            {
                if (!m_Failed)
                {
                    m_Errors.push_back({at.Line, at.Column, message});
                }
                m_Failed = true;
            }
            bool IsPunct(const char* p) const
            {
                return m_Cur.Kind == Tok::Punct && m_Cur.Text == p;
            }
            bool Expect(const char* p)
            {
                if (IsPunct(p))
                {
                    Advance();
                    return true;
                }
                Fail(m_Cur, std::string("'") + p + "' が必要です");
                return false;
            }
            NodePtr Make(Op op, const Token& at, int dims)
            {
                auto n = std::make_unique<Node>();
                n->Kind = op;
                n->Dims = dims;
                n->Line = at.Line;
                n->Column = at.Column;
                return n;
            }

            // 二項演算の型：同じ次元、または片方がスカラー
            bool BinaryDims(const Token& at, const Node& a, const Node& b, int& dims)
            {
                if (a.Dims == b.Dims)
                {
                    dims = a.Dims;
                    return true;
                }
                if (a.Dims == 1 || b.Dims == 1)
                {
                    dims = (std::max)(a.Dims, b.Dims);
                    return true;
                }
                Fail(at, "型が合いません: float" + std::to_string(a.Dims) + " と float" + std::to_string(b.Dims) + "（スカラーとだけ混ぜられます）");
                return false;
            }

            NodePtr ParseTernary()
            {
                NodePtr cond = ParseOr();
                if (m_Failed || !IsPunct("?"))
                {
                    return cond;
                }
                const Token at = m_Cur;
                Advance();
                NodePtr a = ParseTernary();
                if (m_Failed || !Expect(":"))
                {
                    return nullptr;
                }
                NodePtr b = ParseTernary();
                if (m_Failed)
                {
                    return nullptr;
                }
                if (cond->Dims != 1)
                {
                    Fail(at, "?: の条件はスカラーです");
                    return nullptr;
                }
                int dims = 1;
                if (!BinaryDims(at, *a, *b, dims))
                {
                    return nullptr;
                }
                auto n = Make(Op::Ternary, at, dims);
                n->Args.push_back(std::move(cond));
                n->Args.push_back(std::move(a));
                n->Args.push_back(std::move(b));
                return n;
            }
            NodePtr ParseOr()
            {
                NodePtr left = ParseAnd();
                while (!m_Failed && IsPunct("||"))
                {
                    left = Binary(Op::Or, std::move(left), &Parser::ParseAnd, true);
                }
                return left;
            }
            NodePtr ParseAnd()
            {
                NodePtr left = ParseEquality();
                while (!m_Failed && IsPunct("&&"))
                {
                    left = Binary(Op::And, std::move(left), &Parser::ParseEquality, true);
                }
                return left;
            }
            NodePtr ParseEquality()
            {
                NodePtr left = ParseRelational();
                while (!m_Failed && (IsPunct("==") || IsPunct("!=")))
                {
                    const Op op = IsPunct("==") ? Op::Equal : Op::NotEqual;
                    left = Binary(op, std::move(left), &Parser::ParseRelational, true);
                }
                return left;
            }
            NodePtr ParseRelational()
            {
                NodePtr left = ParseAdditive();
                while (!m_Failed && (IsPunct("<") || IsPunct(">") || IsPunct("<=") || IsPunct(">=")))
                {
                    const Op op = IsPunct("<") ? Op::Less : IsPunct(">") ? Op::Greater : IsPunct("<=") ? Op::LessEq : Op::GreaterEq;
                    left = Binary(op, std::move(left), &Parser::ParseAdditive, true);
                }
                return left;
            }
            NodePtr ParseAdditive()
            {
                NodePtr left = ParseMultiplicative();
                while (!m_Failed && (IsPunct("+") || IsPunct("-")))
                {
                    const Op op = IsPunct("+") ? Op::Add : Op::Sub;
                    left = Binary(op, std::move(left), &Parser::ParseMultiplicative, false);
                }
                return left;
            }
            NodePtr ParseMultiplicative()
            {
                NodePtr left = ParseUnary();
                while (!m_Failed && (IsPunct("*") || IsPunct("/")))
                {
                    const Op op = IsPunct("*") ? Op::Mul : Op::Div;
                    left = Binary(op, std::move(left), &Parser::ParseUnary, false);
                }
                return left;
            }
            NodePtr Binary(Op op, NodePtr left, NodePtr (Parser::*next)(), bool scalarOnly)
            {
                const Token at = m_Cur;
                Advance();
                NodePtr right = (this->*next)();
                if (m_Failed || !left || !right)
                {
                    return nullptr;
                }
                int dims = 1;
                if (scalarOnly)
                {
                    if (left->Dims != 1 || right->Dims != 1)
                    {
                        Fail(at, "比較 / 論理演算はスカラーだけです");
                        return nullptr;
                    }
                }
                else if (!BinaryDims(at, *left, *right, dims))
                {
                    return nullptr;
                }
                auto n = Make(op, at, dims);
                n->Args.push_back(std::move(left));
                n->Args.push_back(std::move(right));
                return n;
            }
            NodePtr ParseUnary()
            {
                if (IsPunct("-") || IsPunct("!") || IsPunct("+"))
                {
                    const Token at = m_Cur;
                    const std::string p = m_Cur.Text;
                    Advance();
                    NodePtr v = ParseUnary();
                    if (m_Failed || !v)
                    {
                        return nullptr;
                    }
                    if (p == "+")
                    {
                        return v;
                    }
                    if (p == "!" && v->Dims != 1)
                    {
                        Fail(at, "! はスカラーだけです");
                        return nullptr;
                    }
                    auto n = Make(p == "-" ? Op::Neg : Op::Not, at, v->Dims);
                    n->Args.push_back(std::move(v));
                    return n;
                }
                return ParsePostfix();
            }
            NodePtr ParsePostfix()
            {
                NodePtr v = ParsePrimary();
                while (!m_Failed && v && IsPunct("."))
                {
                    const Token at = m_Cur;
                    Advance();
                    if (m_Cur.Kind != Tok::Ident)
                    {
                        Fail(m_Cur, "swizzle（x / y / z / w / r / g / b / a）が必要です");
                        return nullptr;
                    }
                    std::string mask = m_Cur.Text;
                    const Token maskToken = m_Cur;
                    Advance();
                    if (mask.empty() || mask.size() > 4)
                    {
                        Fail(maskToken, "swizzle は 1〜4 文字です: " + mask);
                        return nullptr;
                    }
                    std::string normalized;
                    for (char c : mask)
                    {
                        const std::string xyzw = "xyzw";
                        const std::string rgba = "rgba";
                        size_t k = xyzw.find(c);
                        if (k == std::string::npos)
                        {
                            k = rgba.find(c);
                        }
                        if (k == std::string::npos || static_cast<int>(k) >= v->Dims)
                        {
                            Fail(maskToken, "swizzle '" + mask + "' は float" + std::to_string(v->Dims) + " に使えません");
                            return nullptr;
                        }
                        normalized += xyzw[k];
                    }
                    auto n = Make(Op::Swizzle, at, static_cast<int>(normalized.size()));
                    n->Mask = normalized;
                    n->Args.push_back(std::move(v));
                    v = std::move(n);
                }
                return v;
            }
            NodePtr ParsePrimary()
            {
                const Token at = m_Cur;
                if (m_Cur.Kind == Tok::Number)
                {
                    auto n = Make(Op::Number, at, 1);
                    n->Value = m_Cur.Number;
                    Advance();
                    return n;
                }
                if (IsPunct("("))
                {
                    Advance();
                    NodePtr v = ParseTernary();
                    if (m_Failed || !Expect(")"))
                    {
                        return nullptr;
                    }
                    return v;
                }
                if (m_Cur.Kind != Tok::Ident)
                {
                    Fail(m_Cur, m_Cur.Kind == Tok::End ? "式が途中で終わっています" : "式が必要です: " + m_Cur.Text);
                    return nullptr;
                }
                const std::string name = m_Cur.Text;
                Advance();
                if (IsPunct("("))
                {
                    return ParseCall(name, at);
                }
                for (size_t i = 0; i < m_Names.size(); ++i)
                {
                    if (m_Names[i] == name)
                    {
                        auto n = Make(Op::Input, at, i < m_Dims.size() ? std::clamp(m_Dims[i], 1, 4) : 1);
                        n->Index = static_cast<int>(i);
                        n->Name = name;
                        return n;
                    }
                }
                if (name == "PI")
                {
                    auto n = Make(Op::Number, at, 1);
                    n->Value = 3.14159265f;
                    return n;
                }
                if (name == "TWO_PI")
                {
                    auto n = Make(Op::Number, at, 1);
                    n->Value = 6.28318531f;
                    return n;
                }
                Fail(at, "未定義の名前です: " + name + "（入力ピンの名前か、許可した関数だけ使えます）");
                return nullptr;
            }
            NodePtr ParseCall(const std::string& name, const Token& at)
            {
                const int ctor = name == "float" ? 1 : name == "float2" ? 2 : name == "float3" ? 3 : name == "float4" ? 4 : 0;
                const FunctionInfo* info = ctor ? nullptr : FindFunction(name);
                if (!ctor && !info)
                {
                    Fail(at, "許可されていない関数です: " + name + "（許可: " + AllowedFunctionList() + "、float2/3/4）");
                    return nullptr;
                }
                Expect("(");
                std::vector<NodePtr> args;
                if (!IsPunct(")"))
                {
                    for (;;)
                    {
                        NodePtr a = ParseTernary();
                        if (m_Failed || !a)
                        {
                            return nullptr;
                        }
                        args.push_back(std::move(a));
                        if (IsPunct(","))
                        {
                            Advance();
                            continue;
                        }
                        break;
                    }
                }
                if (!Expect(")"))
                {
                    return nullptr;
                }
                if (ctor)
                {
                    int total = 0;
                    for (const auto& a : args)
                    {
                        total += a->Dims;
                    }
                    if (args.size() == 1 && args[0]->Dims == 1)
                    {
                        total = ctor; // float3(x) = (x, x, x)
                    }
                    if (total != ctor)
                    {
                        Fail(at, name + " の成分数が合いません（必要 " + std::to_string(ctor) + "、与えた " + std::to_string(total) + "）");
                        return nullptr;
                    }
                    if (ctor == 1 && args.size() == 1)
                    {
                        return std::move(args[0]);
                    }
                    auto n = Make(Op::Construct, at, ctor);
                    n->Args = std::move(args);
                    return n;
                }
                if (static_cast<int>(args.size()) != info->Arity)
                {
                    Fail(at, name + " の引数は " + std::to_string(info->Arity) + " 個です");
                    return nullptr;
                }
                int dims = 1;
                switch (info->Kind)
                {
                    case 0:
                        dims = args[0]->Dims;
                        break;
                    case 1:
                        if (name == "pow")
                        {
                            if (args[1]->Dims != 1 && args[1]->Dims != args[0]->Dims)
                            {
                                Fail(at, "pow の指数はスカラーか同じ次元です");
                                return nullptr;
                            }
                            dims = args[0]->Dims;
                        }
                        else if (!BinaryDims(at, *args[0], *args[1], dims))
                        {
                            return nullptr;
                        }
                        break;
                    case 2: {
                        if (name == "lerp")
                        {
                            if (!BinaryDims(at, *args[0], *args[1], dims))
                            {
                                return nullptr;
                            }
                            if (args[2]->Dims != 1 && args[2]->Dims != dims)
                            {
                                Fail(at, "lerp の係数はスカラーか同じ次元です");
                                return nullptr;
                            }
                        }
                        else if (name == "smoothstep")
                        {
                            int d01 = 1;
                            if (!BinaryDims(at, *args[0], *args[1], d01) || !BinaryDims(at, *args[2], *args[0], dims))
                            {
                                return nullptr;
                            }
                            dims = (std::max)(d01, args[2]->Dims);
                        }
                        else
                        {
                            int d01 = 1;
                            if (!BinaryDims(at, *args[1], *args[2], d01) || !BinaryDims(at, *args[0], *args[1], dims))
                            {
                                return nullptr;
                            }
                            dims = (std::max)({dims, d01, args[0]->Dims});
                        }
                        break;
                    }
                    default:
                        if (name == "dot" || name == "distance")
                        {
                            if (args[0]->Dims != args[1]->Dims)
                            {
                                Fail(at, name + " の引数は同じ次元です");
                                return nullptr;
                            }
                            dims = 1;
                        }
                        else if (name == "cross")
                        {
                            if (args[0]->Dims != 3 || args[1]->Dims != 3)
                            {
                                Fail(at, "cross は float3 だけです");
                                return nullptr;
                            }
                            dims = 3;
                        }
                        else if (name == "length")
                        {
                            dims = 1;
                        }
                        else // normalize
                        {
                            dims = args[0]->Dims;
                        }
                        break;
                }
                auto n = Make(Op::Call, at, dims);
                n->Name = name;
                n->Args = std::move(args);
                return n;
            }

            Lexer m_Lexer;
            Token m_Cur;
            const std::vector<std::string>& m_Names;
            const std::vector<int>& m_Dims;
            std::vector<Diagnostic>& m_Errors;
            bool m_Failed = false;
        };

        // ------------------------------------------------------------------ 評価
        struct Val
        {
            float V[4]{};
            int N = 1;
        };
        Val Splat(float x, int n)
        {
            Val v;
            v.N = n;
            for (int i = 0; i < 4; ++i)
            {
                v.V[i] = x;
            }
            return v;
        }
        float Comp(const Val& v, int i)
        {
            return v.N == 1 ? v.V[0] : v.V[std::min(i, v.N - 1)];
        }
        float Safe(float v)
        {
            return std::isfinite(v) ? v : 0.0f;
        }

        float Unary1(const std::string& f, float x)
        {
            if (f == "abs") return std::fabs(x);
            if (f == "ceil") return std::ceil(x);
            if (f == "floor") return std::floor(x);
            if (f == "frac") return x - std::floor(x);
            if (f == "round") return std::nearbyint(x); // HLSL の round は偶数丸め（VM / 生成 HLSL と同じ）
            if (f == "trunc") return std::trunc(x);
            if (f == "sign") return x > 0 ? 1.0f : (x < 0 ? -1.0f : 0.0f);
            if (f == "saturate") return std::clamp(x, 0.0f, 1.0f);
            if (f == "sqrt") return std::sqrt((std::max)(x, 0.0f));
            if (f == "sin") return std::sin(x);
            if (f == "cos") return std::cos(x);
            if (f == "tan") return std::tan(x);
            if (f == "asin") return std::asin(std::clamp(x, -1.0f, 1.0f));
            if (f == "acos") return std::acos(std::clamp(x, -1.0f, 1.0f));
            if (f == "atan") return std::atan(x);
            if (f == "exp") return std::exp(std::clamp(x, -80.0f, 80.0f));
            if (f == "log2") return std::log2((std::max)(x, 1e-30f));
            return x;
        }

        Val Eval(const Node& n, const std::vector<std::array<float, 4>>& inputs)
        {
            switch (n.Kind)
            {
                case Op::Number:
                    return Splat(n.Value, 1);
                case Op::Input: {
                    Val v;
                    v.N = n.Dims;
                    if (n.Index >= 0 && n.Index < static_cast<int>(inputs.size()))
                    {
                        for (int i = 0; i < 4; ++i)
                        {
                            v.V[i] = Safe(inputs[static_cast<size_t>(n.Index)][static_cast<size_t>(i)]);
                        }
                    }
                    return v;
                }
                case Op::Neg: {
                    Val v = Eval(*n.Args[0], inputs);
                    for (float& c : v.V) c = -c;
                    return v;
                }
                case Op::Not:
                    return Splat(Eval(*n.Args[0], inputs).V[0] == 0.0f ? 1.0f : 0.0f, 1);
                case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: {
                    const Val a = Eval(*n.Args[0], inputs);
                    const Val b = Eval(*n.Args[1], inputs);
                    Val r;
                    r.N = n.Dims;
                    for (int i = 0; i < 4; ++i)
                    {
                        const float x = Comp(a, i);
                        const float y = Comp(b, i);
                        r.V[i] = n.Kind == Op::Add ? x + y : n.Kind == Op::Sub ? x - y : n.Kind == Op::Mul ? x * y : (std::fabs(y) < 1e-8f ? 0.0f : x / y);
                    }
                    return r;
                }
                case Op::Less: case Op::Greater: case Op::LessEq: case Op::GreaterEq: case Op::Equal: case Op::NotEqual: case Op::And: case Op::Or: {
                    const float x = Eval(*n.Args[0], inputs).V[0];
                    const float y = Eval(*n.Args[1], inputs).V[0];
                    bool r = false;
                    switch (n.Kind)
                    {
                        case Op::Less: r = x < y; break;
                        case Op::Greater: r = x > y; break;
                        case Op::LessEq: r = x <= y; break;
                        case Op::GreaterEq: r = x >= y; break;
                        case Op::Equal: r = x == y; break;
                        case Op::NotEqual: r = x != y; break;
                        case Op::And: r = x != 0.0f && y != 0.0f; break;
                        default: r = x != 0.0f || y != 0.0f; break;
                    }
                    return Splat(r ? 1.0f : 0.0f, 1);
                }
                case Op::Ternary: {
                    // 両方を評価する（VM / HLSL の select と同じ）
                    const float c = Eval(*n.Args[0], inputs).V[0];
                    const Val a = Eval(*n.Args[1], inputs);
                    const Val b = Eval(*n.Args[2], inputs);
                    Val r;
                    r.N = n.Dims;
                    for (int i = 0; i < 4; ++i)
                    {
                        r.V[i] = c != 0.0f ? Comp(a, i) : Comp(b, i);
                    }
                    return r;
                }
                case Op::Swizzle: {
                    const Val a = Eval(*n.Args[0], inputs);
                    Val r;
                    r.N = n.Dims;
                    for (size_t i = 0; i < n.Mask.size(); ++i)
                    {
                        r.V[i] = a.V[std::string("xyzw").find(n.Mask[i])];
                    }
                    return r;
                }
                case Op::Construct: {
                    Val r;
                    r.N = n.Dims;
                    int k = 0;
                    if (n.Args.size() == 1 && n.Args[0]->Dims == 1)
                    {
                        return Splat(Eval(*n.Args[0], inputs).V[0], n.Dims);
                    }
                    for (const auto& a : n.Args)
                    {
                        const Val v = Eval(*a, inputs);
                        for (int i = 0; i < v.N && k < 4; ++i)
                        {
                            r.V[k++] = v.V[i];
                        }
                    }
                    return r;
                }
                case Op::Call: {
                    const std::string& f = n.Name;
                    std::vector<Val> args;
                    for (const auto& a : n.Args)
                    {
                        args.push_back(Eval(*a, inputs));
                    }
                    Val r;
                    r.N = n.Dims;
                    const FunctionInfo* info = FindFunction(f);
                    if (info->Kind == 0)
                    {
                        for (int i = 0; i < 4; ++i) r.V[i] = Unary1(f, args[0].V[i]);
                    }
                    else if (info->Kind == 1)
                    {
                        for (int i = 0; i < 4; ++i)
                        {
                            const float x = Comp(args[0], i);
                            const float y = f == "pow" ? Comp(args[1], i) : Comp(args[1], i);
                            float v = 0.0f;
                            if (f == "pow") v = std::pow((std::max)(x, 0.0f), y);
                            else if (f == "min") v = (std::min)(x, y);
                            else if (f == "max") v = (std::max)(x, y);
                            else if (f == "fmod") v = std::fabs(y) < 1e-8f ? 0.0f : std::fmod(x, y);
                            else if (f == "step") v = x <= y ? 1.0f : 0.0f; // HLSL: step(a, b) = b >= a
                            else if (f == "atan2") v = std::atan2(x, y);
                            r.V[i] = v;
                        }
                    }
                    else if (info->Kind == 2)
                    {
                        for (int i = 0; i < 4; ++i)
                        {
                            const float a = Comp(args[0], i), b = Comp(args[1], i), c = Comp(args[2], i);
                            if (f == "lerp") r.V[i] = a + (b - a) * c;
                            else if (f == "clamp") r.V[i] = (std::min)((std::max)(a, b), c);
                            else
                            {
                                // smoothstep(edge0 = a, edge1 = b, x = c)
                                const float d = b - a;
                                const float t = std::clamp(std::fabs(d) < 1e-12f ? (c >= b ? 1.0f : 0.0f) : (c - a) / d, 0.0f, 1.0f);
                                r.V[i] = t * t * (3.0f - 2.0f * t);
                            }
                        }
                    }
                    else if (f == "dot" || f == "distance" || f == "length")
                    {
                        float sum = 0.0f;
                        for (int i = 0; i < args[0].N; ++i)
                        {
                            const float d = f == "distance" ? args[0].V[i] - args[1].V[i] : f == "length" ? args[0].V[i] : args[0].V[i] * args[1].V[i];
                            sum += f == "dot" ? d : d * d;
                        }
                        r = Splat(f == "dot" ? sum : std::sqrt(sum), 1);
                    }
                    else if (f == "cross")
                    {
                        const auto& a = args[0].V;
                        const auto& b = args[1].V;
                        r.V[0] = a[1] * b[2] - a[2] * b[1];
                        r.V[1] = a[2] * b[0] - a[0] * b[2];
                        r.V[2] = a[0] * b[1] - a[1] * b[0];
                    }
                    else // normalize
                    {
                        float l2 = 0.0f;
                        for (int i = 0; i < args[0].N; ++i) l2 += args[0].V[i] * args[0].V[i];
                        const float inv = l2 > 1e-12f ? 1.0f / std::sqrt(l2) : 0.0f;
                        for (int i = 0; i < 4; ++i) r.V[i] = args[0].V[i] * inv;
                    }
                    for (float& c : r.V) c = Safe(c);
                    return r;
                }
            }
            return {};
        }

        std::string F(float v)
        {
            std::ostringstream s;
            s.imbue(std::locale::classic());
            s << std::setprecision(9) << std::scientific << v << 'f';
            return s.str();
        }
        std::string Vec(int dims)
        {
            return dims == 1 ? "float" : "float" + std::to_string(dims);
        }
        // 次元を揃える（スカラー → N）
        std::string Widen(const std::string& code, int from, int to)
        {
            return from == to ? code : (from == 1 ? "((" + code + ")." + std::string(static_cast<size_t>(to), 'x') + ")" : code);
        }

        std::string Emit(const Node& n, const std::vector<std::string>& in)
        {
            const auto arg = [&](size_t i) { return Emit(*n.Args[i], in); };
            switch (n.Kind)
            {
                case Op::Number:
                    return F(n.Value);
                case Op::Input:
                    return "(" + (n.Index < static_cast<int>(in.size()) ? in[static_cast<size_t>(n.Index)] : std::string("0")) + ")";
                case Op::Neg:
                    return "(-" + arg(0) + ")";
                case Op::Not:
                    return "(" + arg(0) + " == 0.0f ? 1.0f : 0.0f)";
                case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: {
                    const char* op = n.Kind == Op::Add ? "+" : n.Kind == Op::Sub ? "-" : n.Kind == Op::Mul ? "*" : "/";
                    std::string a = Widen(arg(0), n.Args[0]->Dims, n.Dims);
                    std::string b = Widen(arg(1), n.Args[1]->Dims, n.Dims);
                    if (n.Kind == Op::Div)
                    {
                        // 0 除算は 0（CPU / VM と同じ）
                        return "((" + a + ") * step(1e-8f, abs(" + b + ")) / ((" + b + ") + (1.0f - step(1e-8f, abs(" + b + ")))))";
                    }
                    return "(" + a + std::string(op) + b + ")";
                }
                case Op::Less: return "((" + arg(0) + " < " + arg(1) + ") ? 1.0f : 0.0f)";
                case Op::Greater: return "((" + arg(0) + " > " + arg(1) + ") ? 1.0f : 0.0f)";
                case Op::LessEq: return "((" + arg(0) + " <= " + arg(1) + ") ? 1.0f : 0.0f)";
                case Op::GreaterEq: return "((" + arg(0) + " >= " + arg(1) + ") ? 1.0f : 0.0f)";
                case Op::Equal: return "((" + arg(0) + " == " + arg(1) + ") ? 1.0f : 0.0f)";
                case Op::NotEqual: return "((" + arg(0) + " != " + arg(1) + ") ? 1.0f : 0.0f)";
                case Op::And: return "((" + arg(0) + " != 0.0f && " + arg(1) + " != 0.0f) ? 1.0f : 0.0f)";
                case Op::Or: return "((" + arg(0) + " != 0.0f || " + arg(1) + " != 0.0f) ? 1.0f : 0.0f)";
                case Op::Ternary:
                    return "((" + arg(0) + " != 0.0f) ? " + Widen(arg(1), n.Args[1]->Dims, n.Dims) + " : " +
                           Widen(arg(2), n.Args[2]->Dims, n.Dims) + ")";
                case Op::Swizzle:
                    return "(" + arg(0) + ")." + n.Mask;
                case Op::Construct: {
                    if (n.Args.size() == 1 && n.Args[0]->Dims == 1)
                    {
                        return Widen(arg(0), 1, n.Dims);
                    }
                    std::string s = Vec(n.Dims) + "(";
                    for (size_t i = 0; i < n.Args.size(); ++i)
                    {
                        s += (i ? "," : "") + arg(i);
                    }
                    return s + ")";
                }
                case Op::Call: {
                    const std::string& f = n.Name;
                    const FunctionInfo* info = FindFunction(f);
                    if (info->Kind == 0)
                    {
                        if (f == "trunc" || f == "round" || f == "sign")
                        {
                            return f + "(" + arg(0) + ")";
                        }
                        if (f == "sqrt") return "sqrt(max(" + arg(0) + ",0.0f))";
                        if (f == "exp") return "exp(clamp(" + arg(0) + ",-80.0f,80.0f))";
                        if (f == "log2") return "log2(max(" + arg(0) + ",1e-30f))";
                        if (f == "asin" || f == "acos") return f + "(clamp(" + arg(0) + ",-1.0f,1.0f))";
                        return f + "(" + arg(0) + ")";
                    }
                    if (info->Kind == 1)
                    {
                        const std::string a = Widen(arg(0), n.Args[0]->Dims, n.Dims);
                        const std::string b = f == "pow" ? Widen(arg(1), n.Args[1]->Dims, n.Dims) : Widen(arg(1), n.Args[1]->Dims, n.Dims);
                        if (f == "pow") return "pow(max(" + a + ",0.0f)," + b + ")";
                        if (f == "step") return "step(" + a + "," + b + ")";
                        if (f == "fmod")
                        {
                            const std::string ok = "step(1e-8f, abs(" + b + "))";
                            return "(fmod(" + a + ", (" + b + ") + (1.0f - " + ok + ")) * " + ok + ")";
                        }
                        return f + "(" + a + "," + b + ")";
                    }
                    if (info->Kind == 2)
                    {
                        return f + "(" + Widen(arg(0), n.Args[0]->Dims, n.Dims) + "," + Widen(arg(1), n.Args[1]->Dims, n.Dims) + "," +
                               Widen(arg(2), n.Args[2]->Dims, n.Dims) + ")";
                    }
                    return f + "(" + arg(0) + (n.Args.size() > 1 ? "," + arg(1) : std::string()) + ")";
                }
            }
            return "0.0f";
        }
    } // namespace

    std::string Diagnostic::Format() const
    {
        return "(" + std::to_string(Line) + ":" + std::to_string(Column) + ") " + Message;
    }

    std::string CompileResult::FirstError() const
    {
        return Errors.empty() ? std::string() : Errors.front().Format();
    }

    bool IsAllowedFunction(const std::string& name)
    {
        return FindFunction(name) != nullptr || name == "float" || name == "float2" || name == "float3" || name == "float4";
    }

    const char* AllowedFunctionList()
    {
        return "abs ceil floor frac round trunc sign saturate sqrt sin cos tan asin acos atan exp log2 pow min max fmod step atan2 "
               "lerp clamp smoothstep dot cross length distance normalize";
    }

    CompileResult Compile(const std::string& source, const std::vector<std::string>& inputNames, const std::vector<int>& inputDims,
                          int outputDims)
    {
        CompileResult result;
        if (source.size() > 4096)
        {
            result.Errors.push_back({1, 1, "式が長すぎます（4096 文字まで）"});
            return result;
        }
        auto program = std::make_shared<Program>();
        program->Source = source;
        program->InputNames = inputNames;
        program->InputDims = inputDims;
        Parser parser(source, inputNames, inputDims, result.Errors);
        NodePtr root = parser.ParseProgram();
        if (!result.Errors.empty() || !root)
        {
            if (result.Errors.empty())
            {
                result.Errors.push_back({1, 1, "式を解析できません"});
            }
            return result;
        }
        outputDims = std::clamp(outputDims, 1, 4);
        if (root->Dims != outputDims && root->Dims != 1)
        {
            result.Errors.push_back({root->Line, root->Column,
                                     "式の型 float" + std::to_string(root->Dims) + " が出力の型 float" + std::to_string(outputDims) +
                                         " と合いません（スカラーは自動で広げます）"});
            return result;
        }
        program->OutputDims = outputDims;
        program->Root = std::move(root);
        result.Result = std::move(program);
        return result;
    }

    void Evaluate(const Program& program, const std::vector<std::array<float, 4>>& inputs, float* out)
    {
        for (int i = 0; i < 4; ++i)
        {
            out[i] = 0.0f;
        }
        if (!program.Root)
        {
            return;
        }
        const Val v = Eval(*program.Root, inputs);
        for (int i = 0; i < 4; ++i)
        {
            out[i] = Safe(Comp(v, i));
        }
    }

    std::string EmitHlsl(const Program& program, const std::vector<std::string>& inputExpressions)
    {
        if (!program.Root)
        {
            return "0.0f";
        }
        return Widen(Emit(*program.Root, inputExpressions), program.Root->Dims, program.OutputDims);
    }
} // namespace MaterialGraphCustomHlsl
