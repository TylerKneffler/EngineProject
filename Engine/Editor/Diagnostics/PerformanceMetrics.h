#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
namespace Engine::Editor
{
struct FrameStatistics
{
    double meanMs=0, p95Ms=0, p99Ms=0, fps=0, over60BudgetPercent=0;
};
inline FrameStatistics SummarizeFrames(std::vector<double> values)
{
    values.erase(std::remove_if(values.begin(),values.end(),[](double v){return !std::isfinite(v)||v<=0;}),values.end());
    FrameStatistics result;
    if(values.empty()) return result;
    for(double v:values) { result.meanMs+=v; result.over60BudgetPercent+=v>1000./60.; }
    result.meanMs/=values.size(); result.fps=1000./result.meanMs;
    result.over60BudgetPercent*=100./values.size();
    std::sort(values.begin(),values.end());
    result.p95Ms=values[size_t(std::ceil(.95*values.size()))-1];
    result.p99Ms=values[size_t(std::ceil(.99*values.size()))-1];
    return result;
}
}
