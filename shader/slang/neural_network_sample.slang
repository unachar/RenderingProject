// -----------------------------------------------------------------------------------
// Neural Network (Multi-Layer Perceptron) Sample Compute Shader in Slang
// -----------------------------------------------------------------------------------
// 概要:
// シンプルなMLP (Multi-Layer Perceptron) のフォワードパスを実行するコンピュートシェーダーのサンプルです。
// Slangの強力な言語機能 (struct, メソッド, インターフェース) を活用して、ニューラルネットワーク層を表現しています。
// 実際の利用時には、C++側から重みとバイアスの事前計算済みバッファを渡し、
// 各スレッド（またはウェーブ単位）で推論を実行させます。

// 活性化関数（ReLUとSigmoid）
float relu(float x) { return max(0.0f, x); }
float sigmoid(float x) { return 1.0f / (1.0f + exp(-x)); }

// ネットワークパラメータの構造体
struct MLPLayer
{
    // C++側からは StructuredBuffer などを通して渡すことを想定
    float weights[16][16]; // [input_dim][output_dim]
    float biases[16];      // [output_dim]
    int in_features;
    int out_features;

    // 層のフォワードコンピュート（行列とベクトルの積 + バイアス）
    // outParams は関数引数として出力配列のポインタのように渡して結果を受け取る
    void forward(float inData[16], out float outData[16])
    {
        for (int i = 0; i < out_features; i++)
        {
            float sum = biases[i];
            for (int j = 0; j < in_features; j++)
            {
                sum += inData[j] * weights[j][i];
            }
            outData[i] = sum;
        }
    }
};

// 2層のMLPモデル全体
struct SimpleNeuralNetwork
{
    MLPLayer hiddenLayer; // 隠れ層
    MLPLayer outputLayer; // 出力層

    // フォワードパスの実行
    void predict(float inputFeatures[16], out float outputFeatures[16], int outCount)
    {
        float hiddenOut[16];
        
        // 1. 隠れ層の計算
        hiddenLayer.forward(inputFeatures, hiddenOut);
        
        // 2. 隠れ層への活性化関数適用 (ReLU)
        for (int i = 0; i < hiddenLayer.out_features; i++)
        {
            hiddenOut[i] = relu(hiddenOut[i]);
        }
        
        // 3. 出力層の計算
        outputLayer.forward(hiddenOut, outputFeatures);
        
        // 4. 出力層への活性化関数適用 (今回はSigmoidと仮定)
        for (int j = 0; j < outCount; j++)
        {
            outputFeatures[j] = sigmoid(outputFeatures[j]);
        }
    }
};

// --- パラメータ受け渡しブロック ---
struct ShaderParams
{
    uint numElements;   // 処理するデータ数
};
ParameterBlock<ShaderParams> g_Params;

// 重みとモデル構造が入った定数バッファ
ConstantBuffer<SimpleNeuralNetwork> g_ModelData;

// 入力データと出力データ
// 例えば、ピクセルバッファやパーティクル情報など
StructuredBuffer<float> g_InputData;    // 1スレッドあたり16要素を扱うなど
RWStructuredBuffer<float> g_OutputData; // 出力先

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 threadId : SV_DispatchThreadID)
{
    uint index = threadId.x;
    
    // スレッドが担当するデータが範囲外なら終了
    if (index >= g_Params.numElements)
        return;

    // 1. 入力データのフェッチ (1スレッドにつき 16次元の入力を取得すると仮定)
    float inputFeatures[16];
    for(int i = 0; i < 16; i++)
    {
        // 簡易的に連続するメモリから取得する例
        inputFeatures[i] = g_InputData[index * 16 + i];
    }
    
    // 2. ネットワークを実行して推論
    float outputFeatures[16];
    g_ModelData.predict(inputFeatures, outputFeatures, g_ModelData.outputLayer.out_features);
    
    // 3. 結果の保存 (出力次元数が4の場合として仮定)
    for(int j = 0; j < g_ModelData.outputLayer.out_features; j++)
    {
         g_OutputData[index * 16 + j] = outputFeatures[j];
    }
}
