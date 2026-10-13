#include "Editor/Core/AnimationClipEditing.h"
#include <cassert>
#include <cmath>

using Engine::Editor::AnimationClipEditing::CopyKeyBlocks;
using Engine::Editor::AnimationClipEditing::PasteKeyBlocks;
using Engine::Model::AnimationChannel;
using Engine::Model::AnimationClip;

int main()
{
    AnimationClip source;
    AnimationChannel translation;
    translation.nodeIndex = 4;
    translation.targetPath = "Root/Arm";
    translation.path = AnimationChannel::Path::Translation;
    translation.valueWidth = 3;
    translation.interpolation = AnimationChannel::Interpolation::CubicSpline;
    translation.times = { 1.f, 2.f };
    translation.values = {
        1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f,
        10.f, 11.f, 12.f, 13.f, 14.f, 15.f, 16.f, 17.f, 18.f };
    source.channels.push_back(translation);

    auto first = CopyKeyBlocks(source, 4, 1.f, 0.f);
    auto second = CopyKeyBlocks(source, 4, 2.f, 1.f);
    assert(first.size() == 1 && second.size() == 1);
    first.insert(first.end(), second.begin(), second.end());
    assert(first[0].values.size() == 9);
    assert(first[0].values[3] == 4.f);

    AnimationClip destination;
    assert(PasteKeyBlocks(destination, first, 3.f));
    assert(destination.channels.size() == 1);
    const auto& pasted = destination.channels.front();
    assert(pasted.interpolation == AnimationChannel::Interpolation::CubicSpline);
    assert(pasted.targetPath == "Root/Arm");
    assert(pasted.times == std::vector<float>({ 3.f, 4.f }));
    assert(pasted.values == translation.values);
    assert(destination.duration == 4.f);

    auto replacement = first;
    replacement[0].values[3] = 42.f;
    assert(PasteKeyBlocks(destination, replacement, 3.f));
    assert(destination.channels.front().times.size() == 2);
    assert(destination.channels.front().values[3] == 42.f);
    assert(source.channels.front().values[3] == 4.f);

    auto malformed = first;
    malformed[0].values.pop_back();
    const auto before = destination.channels.front().values;
    assert(!PasteKeyBlocks(destination, malformed, 8.f));
    assert(destination.channels.front().values == before);
}
