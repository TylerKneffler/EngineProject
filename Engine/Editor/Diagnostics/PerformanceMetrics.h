#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include <deque>
namespace Engine::Editor
{
struct FrameStatistics
{
    double meanMs=0, p95Ms=0, p99Ms=0, fps=0, over60BudgetPercent=0;
};
// Count editor frame intervals, independently of panel draws or input events.
class FrameRateWindow
{
    std::deque<double> m_intervals;
    double m_total=0;
public:
    void Add(double intervalMs)
    {
        if(!std::isfinite(intervalMs) || intervalMs<=0) return;
        m_intervals.push_back(intervalMs); m_total+=intervalMs;
        while(m_intervals.size()>1 && (m_total-m_intervals.front()>=500. || m_intervals.size()>600))
        { m_total-=m_intervals.front(); m_intervals.pop_front(); }
    }
    double Fps() const { return m_total>0 ? 1000.*m_intervals.size()/m_total:0; }
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
