#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <vector>
#include <deque>
namespace Engine::Editor
{
struct FrameStatistics
{
    double meanMs=0, p95Ms=0, p99Ms=0, fps=0, over60BudgetPercent=0;
};
// Include zero-event intervals so an idle renderer reports zero throughput.
class FrameRateWindow
{
    struct Sample { double milliseconds; uint64_t count; };
    std::deque<Sample> m_samples;
    double m_total=0;
    uint64_t m_count=0;
public:
    void Add(double intervalMs, uint64_t count=1)
    {
        if(!std::isfinite(intervalMs) || intervalMs<=0) return;
        m_samples.push_back({intervalMs,count}); m_total+=intervalMs; m_count+=count;
        while(m_samples.size()>1 && (m_total-m_samples.front().milliseconds>=500. || m_samples.size()>600))
        {
            m_total-=m_samples.front().milliseconds;
            m_count-=m_samples.front().count;
            m_samples.pop_front();
        }
    }
    double Fps() const { return m_total>0 ? 1000.*m_count/m_total:0; }
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
