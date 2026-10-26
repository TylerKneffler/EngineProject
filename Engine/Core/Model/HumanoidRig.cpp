#include "HumanoidRig.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <functional>
#include <limits>
#include <unordered_map>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Engine::Model::HumanoidRig
{
bool Mapping::HasAssignments() const
{
    return std::any_of(bones.begin(), bones.end(), [](int node) { return node >= 0; });
}
Engine::Serialization::JsonValue Mapping::Serialize() const
{
    using J = Engine::Serialization::JsonValue;
    auto roles = J::MakeObject();
    for (size_t i = 0; i < Count; ++i) roles.Set(Keys[i], J(bones[i]));
    return J::MakeObject().Set("version", J(1)).Set("upAxis", J(upAxis))
        .Set("forwardAxis", J(forwardAxis)).Set("leftPositive", J(leftPositive))
        .Set("bones", std::move(roles));
}
Mapping Mapping::Deserialize(const Engine::Serialization::JsonValue& value)
{
    Mapping result;
    if (!value.IsObject()) return result;
    if (value["upAxis"].IsNumber()) result.upAxis = value["upAxis"].AsInt();
    if (value["forwardAxis"].IsNumber()) result.forwardAxis = value["forwardAxis"].AsInt();
    if (value["leftPositive"].IsBool()) result.leftPositive = value["leftPositive"].AsBool();
    for (size_t i = 0; i < Count; ++i)
        if (value["bones"][Keys[i]].IsNumber()) result.bones[i] = value["bones"][Keys[i]].AsInt();
    return result;
}

std::vector<Node> BuildNodes(const std::vector<RestTransform>& input,
    const std::vector<unsigned>& joints)
{
    std::vector<Node> nodes(input.size());
    std::unordered_map<std::string, int> indices;
    for (size_t i = 0; i < input.size(); ++i)
        if (!indices.emplace(input[i].path, int(i)).second) return {};
    std::vector<glm::mat4> world(input.size(), glm::mat4(1.f));
    std::vector<int> visits(input.size());
    const auto root = indices.find(".");
    for (size_t i = 0; i < input.size(); ++i)
    {
        const auto found = indices.find(input[i].parent);
        const auto slash = input[i].path.find_last_of('/');
        nodes[i].name = input[i].path.substr(slash == std::string::npos ? 0 : slash + 1);
        if (found != indices.end()) nodes[i].parent = found->second;
        else if (input[i].parent.empty() && root != indices.end() && root->second != int(i))
            nodes[i].parent = root->second;
        else if (!input[i].parent.empty()) return {};
        nodes[i].joint = joints.empty() && input[i].path != "." &&
            input[i].path.find("$AssimpFbx$") == std::string::npos;
    }
    for (unsigned joint : joints) if (joint < nodes.size()) nodes[joint].joint = true;
    std::function<bool(size_t)> visit = [&](size_t index)
    {
        if (visits[index] == 2) return true;
        if (visits[index] == 1) return false;
        visits[index] = 1;
        const auto& source = input[index];
        const auto& e = source.rotation;
        const glm::quat rotation = glm::angleAxis(e.z, glm::vec3(0,0,1)) *
            glm::angleAxis(e.y, glm::vec3(0,1,0)) * glm::angleAxis(e.x, glm::vec3(1,0,0));
        const auto local = glm::translate(glm::mat4(1.f), source.position) *
            glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.f), source.scale);
        const int parent = nodes[index].parent;
        if (parent >= 0 && !visit(size_t(parent))) return false;
        world[index] = parent < 0 ? local : world[parent] * local;
        nodes[index].position = glm::vec3(world[index][3]);
        if (!std::isfinite(nodes[index].position.x) || !std::isfinite(nodes[index].position.y) ||
            !std::isfinite(nodes[index].position.z)) return false;
        visits[index] = 2;
        return true;
    };
    for (size_t i = 0; i < input.size(); ++i) if (!visit(i)) return {};
    return nodes;
}

namespace
{
bool Descendant(const std::vector<Node>& nodes, int child, int ancestor)
{
    for (size_t depth = 0; child >= 0 && size_t(child) < nodes.size() && depth < nodes.size(); ++depth)
    {
        if (child == ancestor) return true;
        child = nodes[child].parent;
    }
    return false;
}
int CommonAncestor(const std::vector<Node>& nodes, int first, int second)
{
    for (size_t depth = 0; first >= 0 && size_t(first) < nodes.size() && depth < nodes.size(); ++depth)
    {
        if (Descendant(nodes, second, first)) return first;
        first = nodes[first].parent;
    }
    return -1;
}
bool AxesValid(const Mapping& map)
{
    return map.upAxis >= 0 && map.upAxis < 3 && map.forwardAxis >= 0 &&
        map.forwardAxis < 3 && map.upAxis != map.forwardAxis;
}
struct Chain { int upper=-1, lower=-1, tip=-1; float score=-1e9f; };

bool HelperCopy(const std::vector<Node>& nodes, const std::vector<glm::vec3>& positions,
    const Chain& helper, const Chain& primary)
{
    const int copy[] = {helper.upper, helper.lower, helper.tip};
    const int original[] = {primary.upper, primary.lower, primary.tip};
    if (nodes[helper.upper].parent != nodes[primary.upper].parent) return false;
    for (int joint = 0; joint < 3; ++joint)
    {
        if (glm::length(positions[copy[joint]] - positions[original[joint]]) > .001f)
            return false;
        std::string name = nodes[copy[joint]].name;
        const auto& base = nodes[original[joint]].name;
        // MMD D bones copy the control chain. Do not infer roles from names or
        // silently choose between two otherwise indistinguishable armatures.
        if (base.empty() || name.empty() || name.back() != 'D') return false;
        name.pop_back();
        while (!name.empty() && (name.back() == ' ' || name.back() == '_')) name.pop_back();
        if (name != base) return false;
    }
    return true;
}
}

Detection Detect(const std::vector<Node>& nodes, const Mapping& settings)
{
    Detection result;
    result.mapping = settings;
    result.mapping.bones.fill(-1);
    if (!AxesValid(settings)) { result.issues.push_back("Choose different Up and Forward axes."); return result; }
    if (nodes.size() < 12) { result.issues.push_back("Not enough rest-pose joints to identify a humanoid."); return result; }
    for (size_t i=0;i<nodes.size();++i)
    {
        int current=int(i);
        for (size_t depth=0;current>=0;++depth)
        {
            if (size_t(current)>=nodes.size() || depth>=nodes.size())
            { result.issues.push_back("Skeleton hierarchy contains a missing parent or cycle."); return result; }
            current=nodes[current].parent;
        }
    }
    const int sideAxis = 3-settings.upAxis-settings.forwardAxis;
    const int scope = settings.bones[Root];
    if (scope >= 0 && size_t(scope) >= nodes.size())
    { result.issues.push_back("The assigned root is missing from this rig."); return result; }
    const auto eligible = [&](int index)
    { return nodes[index].joint && (scope < 0 || Descendant(nodes,index,scope)); };
    glm::vec3 low(1e30f), high(-1e30f);
    size_t joints = 0;
    for (size_t i=0;i<nodes.size();++i) if (eligible(int(i)))
    {
        const auto& node=nodes[i];
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(node.position[axis]))
            { result.issues.push_back("Rest pose contains non-finite positions."); return result; }
        low = glm::min(low,node.position); high = glm::max(high,node.position); ++joints;
    }
    const float height = high[settings.upAxis]-low[settings.upAxis];
    if (joints < 12 || !(height > 1e-6f))
    { result.issues.push_back("The rest pose has no usable upright humanoid height."); return result; }
    const float center = (high[sideAxis]+low[sideAxis])*.5f;
    std::vector<glm::vec3> p(nodes.size());
    std::vector<int> children(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        p[i] = { (nodes[i].position[sideAxis]-center)/height,
            (nodes[i].position[settings.upAxis]-low[settings.upAxis])/height,
            nodes[i].position[settings.forwardAxis]/height };
        if (!settings.leftPositive) p[i].x = -p[i].x;
        if (nodes[i].parent >= 0 && size_t(nodes[i].parent) < nodes.size()) ++children[nodes[i].parent];
    }
    const auto set = [&](Role role, int node, float confidence)
    { result.mapping.bones[role] = node; result.confidence[role] = confidence; };
    const auto chain = [&](bool arm, bool left)
    {
        std::vector<Chain> candidates;
        const float sign = left ? 1.f : -1.f;
        const size_t role = arm ? (left ? LeftUpperArm : RightUpperArm)
            : (left ? LeftThigh : RightThigh);
        for (int upper = 0; upper < int(nodes.size()); ++upper)
        {
            const auto a=p[upper]; const float x=a.x*sign;
            if (settings.bones[role] >= 0 && settings.bones[role] != upper) continue;
            if (!eligible(upper) || (arm ? (a.y < .55f || a.y > .9f || x < .025f || x > .24f)
                : (a.y < .38f || a.y > .67f || x < .01f || x > .22f))) continue;
            for (int lower = 0; lower < int(nodes.size()); ++lower)
            {
                if (!eligible(lower) || lower == upper || !Descendant(nodes,lower,upper)) continue;
                if (settings.bones[role + 1] >= 0 && settings.bones[role + 1] != lower) continue;
                const auto b=p[lower]; const auto first=b-a;
                const float length1=glm::length(first);
                if (length1 < (arm ? .075f : .13f) || length1 > .32f ||
                    (arm ? first.x*sign < .065f : first.y > -.12f)) continue;
                for (int tip = 0; tip < int(nodes.size()); ++tip)
                {
                    if (!eligible(tip) || tip == lower || !Descendant(nodes,tip,lower)) continue;
                    if (settings.bones[role + 2] >= 0 && settings.bones[role + 2] != tip) continue;
                    const auto c=p[tip]; const auto second=c-b;
                    const float length2=glm::length(second);
                    if (length2 < (arm ? .065f : .12f) || length2 > .32f ||
                        (arm ? (second.x*sign < .055f || c.y < .3f) : (second.y > -.1f || c.y > .18f))) continue;
                    if (length1/length2 < .5f || length1/length2 > 2.f) continue;
                    float score = 1.f - std::abs(length1-length2)*2.f;
                    if (arm)
                        score -= std::abs(a.y-.76f)*.7f + std::abs(x-.10f)*1.5f +
                            std::abs(length1-.15f) + std::abs(length2-.15f);
                    else
                        score -= std::abs(a.y-.50f) + std::abs(b.y-.27f) + std::abs(c.y-.04f) +
                            std::abs(a.x-b.x)*2.f + std::abs(b.x-c.x)*2.f;
                    // A hand branches into fingers; a toe's short horizontal
                    // continuation must not displace the ankle/knee chain.
                    if (arm && children[tip] >= 3) score += .12f;
                    if (!arm && std::abs(c.z-b.z) > .08f) score -= .15f;
                    candidates.push_back({upper,lower,tip,score});
                }
            }
        }
        std::stable_sort(candidates.begin(),candidates.end(),[](const auto& a,const auto& b) {return a.score>b.score;});
        if (candidates.empty()) return Chain{};
        auto best=candidates.front();
        // Equally plausible *different branches* require human identification.
        for (size_t i=1;i<candidates.size() && best.score-candidates[i].score<.025f;++i)
            if (!Descendant(nodes,best.upper,candidates[i].upper) &&
                !Descendant(nodes,candidates[i].upper,best.upper))
            {
                if (HelperCopy(nodes, p, candidates[i], best)) continue;
                if (HelperCopy(nodes, p, best, candidates[i])) { best = candidates[i]; continue; }
                return Chain{};
            }
        // Exporters often insert several co-located parent/control joints.
        // Prefer the last joint before the limb actually leaves that position.
        for (int node=0;node<int(nodes.size());++node)
            if (eligible(node) && node!=best.upper &&
                Descendant(nodes,node,best.upper) && Descendant(nodes,best.lower,node) &&
                glm::length(p[node]-p[best.upper])<.005f) best.upper=node;
        return best;
    };
    const auto assignChain = [&](bool arm, bool left)
    {
        const auto best=chain(arm,left);
        const size_t start=arm ? (left?LeftUpperArm:RightUpperArm) : (left?LeftThigh:RightThigh);
        if (best.upper < 0)
        { result.issues.push_back(std::string(left?"Left ":"Right ")+(arm?"arm":"leg")+" chain needs assignment."); return; }
        set(Role(start),best.upper,.8f); set(Role(start+1),best.lower,.8f); set(Role(start+2),best.tip,.8f);
        if (arm)
        {
            int parent=nodes[best.upper].parent;
            while (parent>=0 && size_t(parent)<nodes.size() &&
                glm::length(p[parent]-p[best.upper])<.01f) parent=nodes[parent].parent;
            if (parent>=0 && nodes[parent].joint && std::abs(p[parent].x)>.015f &&
                std::abs(p[parent].x)<std::abs(p[best.upper].x)) set(left?LeftShoulder:RightShoulder,parent,.65f);
        }
        else
        {
            int toe=-1; float distance=.12f;
            for (int i=0;i<int(nodes.size());++i)
                if (eligible(i) && i!=best.tip && Descendant(nodes,i,best.tip))
                {
                    const float d=glm::length(p[i]-p[best.tip]);
                    if (d>.015f && d<distance && std::abs(p[i].y-p[best.tip].y)<.055f)
                    { distance=d;toe=i; }
                }
            if (toe>=0) set(left?LeftToe:RightToe,toe,.65f);
        }
    };
    assignChain(false,true); assignChain(false,false);
    assignChain(true,true); assignChain(true,false);
    const auto& bones=result.mapping.bones;
    if (bones[LeftThigh]>=0 && bones[RightThigh]>=0)
    {
        int pelvis=CommonAncestor(nodes,bones[LeftThigh],bones[RightThigh]);
        while (pelvis>=0 && !nodes[pelvis].joint) pelvis=nodes[pelvis].parent;
        if (pelvis>=0 && p[pelvis].y>.3f && p[pelvis].y<.7f) set(Pelvis,pelvis,.85f);
    }
    if (bones[LeftUpperArm]>=0 && bones[RightUpperArm]>=0)
    {
        int chest=CommonAncestor(nodes,bones[LeftUpperArm],bones[RightUpperArm]);
        while (chest>=0 && !nodes[chest].joint) chest=nodes[chest].parent;
        if (chest>=0 && chest!=bones[Pelvis] && p[chest].y>.55f) set(Chest,chest,.8f);
    }
    if (bones[Pelvis]>=0)
    {
        int root=bones[Pelvis];
        for (int parent=nodes[root].parent;parent>=0;parent=nodes[parent].parent)
            if (nodes[parent].joint) root=parent;
        set(Root,root,.7f);
    }
    if (bones[Chest]>=0)
    {
        int spine=nodes[bones[Chest]].parent;
        while (spine>=0 && (!nodes[spine].joint ||
            glm::length(p[spine]-p[bones[Chest]])<.015f)) spine=nodes[spine].parent;
        if (spine>=0 && spine!=bones[Pelvis] && p[spine].y>.45f) set(Spine,spine,.7f);
        int head=-1; float score=-1e9f;
        for (int i=0;i<int(nodes.size());++i)
            if (eligible(i) && p[i].y>.83f && std::abs(p[i].x)<.045f && Descendant(nodes,i,bones[Chest]))
            {
                const float value=p[i].y + std::min(children[i],8)*.05f;
                if (value>score) {score=value;head=i;}
            }
        if (head>=0)
        {
            set(Head,head,.6f);
            int neck=nodes[head].parent;
            while (neck>=0 && (!nodes[neck].joint || glm::length(p[neck]-p[head])<.01f)) neck=nodes[neck].parent;
            if (neck>=0 && neck!=bones[Chest] && p[neck].y>p[bones[Chest]].y) set(Neck,neck,.65f);
        }
        std::vector<int> torso;
        const float hipHeight=bones[Pelvis]>=0?p[bones[Pelvis]].y:.45f;
        for (int node=bones[Chest];node>=0;node=nodes[node].parent)
            if (eligible(node) && p[node].y>hipHeight+.002f && std::abs(p[node].x)<.06f &&
                (torso.empty() || glm::length(p[node]-p[torso.back()])>.015f)) torso.push_back(node);
        std::reverse(torso.begin(),torso.end());
        if (!torso.empty())
        {
            set(Spine,torso.front(),.7f);
            set(Chest,torso.size()>1?torso[1]:-1,.7f);
            if (torso.size()>2) set(UpperChest,torso.back(),.7f);
        }
    }
    auto issues=Validate(result.mapping,nodes);
    result.issues.insert(result.issues.end(),issues.begin(),issues.end());
    return result;
}

std::vector<std::string> Validate(const Mapping& map, const std::vector<Node>& nodes)
{
    std::vector<std::string> issues;
    if (!AxesValid(map)) issues.push_back("Up and Forward must be different axes.");
    for (size_t role=0;role<Count;++role)
    {
        const int node=map.bones[role];
        if (node<0) continue;
        if (size_t(node)>=nodes.size() || !nodes[node].joint)
            issues.push_back(std::string(Labels[role])+" is not a joint in this rig.");
        for (size_t other=0;other<role;++other)
            if (map.bones[other]==node && !(role==Pelvis && other==Root))
                issues.push_back(std::string(Labels[role])+" reuses the "+Labels[other]+" joint.");
    }
    for (Role role : {Pelvis,Spine,Head,LeftUpperArm,LeftLowerArm,LeftHand,RightUpperArm,RightLowerArm,RightHand,
            LeftThigh,LeftCalf,LeftFoot,RightThigh,RightCalf,RightFoot})
        if (map.bones[role]<0) issues.push_back(std::string(Labels[role])+" is unassigned.");
    for (auto start : {LeftUpperArm,RightUpperArm,LeftThigh,RightThigh})
        for (int i=0;i<2;++i)
        {
            const int first=map.bones[start+i], second=map.bones[start+i+1];
            if (first>=0 && second>=0 && !Descendant(nodes,second,first))
                issues.push_back(std::string(Labels[start+i+1])+" must descend from "+Labels[start+i]+".");
        }
    return issues;
}
}
