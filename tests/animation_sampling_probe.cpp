// Read-only Assimp clip inventory and sampling-policy comparison, not an ACL benchmark.
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

#include "animation_probe_helpers.h"
using namespace AnimationProbe;

int main(int argc, char** argv)
{
    try
    {
        Require(argc >= 3, "usage: animation_sampling_probe <new-report.json> <FBX>...");
        Require(!std::filesystem::exists(argv[1]), "report must be a new file");
        std::ofstream report(argv[1], std::ios::binary);
        Require(static_cast<bool>(report), "report open failed");
        report << "{\"scope\":\"Read-only Assimp LH clip inventory. Current integer-key selection versus key-time interpolation, at 60Hz half-step times. No ACL compression, world-space/skinned vertex error, renderer or FPS measurement.\",\n\"clips\":[\n";
        size_t clipCount = 0;
        for (int file = 2; file < argc; ++file)
        {
            Assimp::Importer importer;
            const aiScene* scene = importer.ReadFile(argv[file], aiProcess_ConvertToLeftHanded);
            Require(scene != nullptr, importer.GetErrorString());
            for (unsigned clip = 0; clip < scene->mNumAnimations; ++clip)
            {
                const aiAnimation& animation = *scene->mAnimations[clip];
                const double rate = animation.mTicksPerSecond > 0 ? animation.mTicksPerSecond : 25;
                const double duration = animation.mDuration / rate;
                Require(std::isfinite(duration) && duration >= 0 && duration <= 1800, "invalid/oversized clip duration");
                uint64_t positionKeys = 0, rotationKeys = 0, scaleKeys = 0, nonUnitChannels = 0;
                uint64_t samples = 0, positionDifferences = 0, rotationDifferences = 0;
                double maxPosition = 0, maxAngle = 0;
                std::string maxPositionNode, maxRotationNode;
                for (unsigned channelIndex = 0; channelIndex < animation.mNumChannels; ++channelIndex)
                {
                    const aiNodeAnim& channel = *animation.mChannels[channelIndex];
                    ValidateTimes(channel.mPositionKeys, channel.mNumPositionKeys);
                    ValidateTimes(channel.mRotationKeys, channel.mNumRotationKeys);
                    ValidateTimes(channel.mScalingKeys, channel.mNumScalingKeys);
                    positionKeys += channel.mNumPositionKeys;
                    rotationKeys += channel.mNumRotationKeys;
                    scaleKeys += channel.mNumScalingKeys;
                    if (NonUnitKeyTimes(channel.mPositionKeys, channel.mNumPositionKeys) ||
                        NonUnitKeyTimes(channel.mRotationKeys, channel.mNumRotationKeys) ||
                        NonUnitKeyTimes(channel.mScalingKeys, channel.mNumScalingKeys)) ++nonUnitChannels;
                    for (size_t sample = 0; (double(sample) + 0.5) / 60 < duration; ++sample)
                    {
                        // Match the current float time/tick conversion and wrap before choosing a key.
                        const float seconds = static_cast<float>((double(sample) + 0.5) / 60);
                        const float time = animation.mDuration > 0 ?
                            std::fmod(seconds * static_cast<float>(rate), static_cast<float>(animation.mDuration)) : seconds * static_cast<float>(rate);
                        const unsigned tickIndex = static_cast<unsigned>(std::max(0.0f, time));
                        const aiVector3D currentPosition = channel.mNumPositionKeys ?
                            channel.mPositionKeys[tickIndex % channel.mNumPositionKeys].mValue : aiVector3D{};
                        const aiQuaternion currentRotation = channel.mNumRotationKeys ?
                            channel.mRotationKeys[tickIndex % channel.mNumRotationKeys].mValue : aiQuaternion(1, 0, 0, 0);
                        const auto difference = Position(channel.mPositionKeys, channel.mNumPositionKeys, time) - currentPosition;
                        const double positionError = difference.Length();
                        const double rotationError = AngleDegrees(currentRotation, Rotation(channel.mRotationKeys, channel.mNumRotationKeys, time));
                        Require(std::isfinite(positionError) && std::isfinite(rotationError), "nonfinite sampled pose");
                        if (positionError > maxPosition) { maxPosition = positionError; maxPositionNode = channel.mNodeName.C_Str(); }
                        if (rotationError > maxAngle) { maxAngle = rotationError; maxRotationNode = channel.mNodeName.C_Str(); }
                        positionDifferences += positionError > 1.0e-5;
                        rotationDifferences += rotationError > 1.0e-3;
                        ++samples;
                    }
                }
                if (clipCount++) report << ",\n";
                report << "{\"file\":" << std::quoted(argv[file]) << ",\"name\":" << std::quoted(animation.mName.C_Str())
                    << ",\"durationTicks\":" << animation.mDuration << ",\"ticksPerSecond\":" << rate
                    << ",\"durationSeconds\":" << duration << ",\"channels\":" << animation.mNumChannels
                    << ",\"positionKeys\":" << positionKeys << ",\"rotationKeys\":" << rotationKeys
                    << ",\"scaleKeys\":" << scaleKeys << ",\"keyArrayBytes\":"
                    << (positionKeys + scaleKeys) * sizeof(aiVectorKey) + rotationKeys * sizeof(aiQuatKey)
                    << ",\"nonUnitTimeChannels\":" << nonUnitChannels << ",\"sampledChannelTimes\":" << samples
                    << ",\"positionDifferent\":" << positionDifferences << ",\"rotationDifferent\":" << rotationDifferences
                    << ",\"maxPositionDifferenceSourceUnits\":" << maxPosition << ",\"maxPositionNode\":" << std::quoted(maxPositionNode)
                    << ",\"maxRotationDifferenceDegrees\":" << maxAngle << ",\"maxRotationNode\":" << std::quoted(maxRotationNode)
                    << ",\"nonUnitTimeDetails\":[";
                bool detailWritten = false;
                for (unsigned channelIndex = 0; channelIndex < animation.mNumChannels; ++channelIndex)
                {
                    const aiNodeAnim& channel = *animation.mChannels[channelIndex];
                    if (!NonUnitKeyTimes(channel.mPositionKeys, channel.mNumPositionKeys) &&
                        !NonUnitKeyTimes(channel.mRotationKeys, channel.mNumRotationKeys) &&
                        !NonUnitKeyTimes(channel.mScalingKeys, channel.mNumScalingKeys)) continue;
                    if (detailWritten) report << ',';
                    detailWritten = true;
                    report << "{\"node\":" << std::quoted(channel.mNodeName.C_Str())
                        << ",\"positionKeys\":" << channel.mNumPositionKeys << ",\"rotationKeys\":" << channel.mNumRotationKeys
                        << ",\"scaleKeys\":" << channel.mNumScalingKeys << '}';
                }
                report << "]}";
            }
        }
        report << "\n],\"clipCount\":" << clipCount << "}\n";
        report.close();
        Require(!report.fail(), "report write failed");
        std::cout << "PASS " << clipCount << " clips inventoried; all sampled comparisons finite\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
