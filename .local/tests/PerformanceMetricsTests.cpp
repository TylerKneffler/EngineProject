#include "Engine/Editor/Diagnostics/PerformanceMetrics.h"
#include <cmath>
#include <iostream>
#include <limits>

int main()
{
    using Engine::Editor::FrameRateWindow;
    auto near=[](double actual,double expected) {
        if(std::abs(actual-expected)>1.e-8) { std::cerr<<actual<<" != "<<expected<<"\n"; return false; }
        return true;
    };
    FrameRateWindow updates, submissions;
    for(int i=0;i<50;++i) { updates.Add(10); submissions.Add(10,i%2==0 ? 1:0); }
    if(!near(updates.Fps(),100) || !near(submissions.Fps(),50)) return 1;
    // The same image can be submitted repeatedly: count submissions, not changes.
    for(int i=0;i<50;++i) submissions.Add(10,1);
    if(!near(submissions.Fps(),100)) return 2;
    // Time keeps advancing while no frames are submitted; stale FPS must decay.
    for(int i=0;i<10;++i) submissions.Add(50,0);
    if(!near(submissions.Fps(),0)) return 3;
    // Weighted throughput is events / elapsed, not the average of inverse intervals.
    FrameRateWindow uneven;
    uneven.Add(100,2); uneven.Add(400,3);
    if(!near(uneven.Fps(),10)) return 4;
    uneven.Add(0,100); uneven.Add(-10,100);
    uneven.Add(std::numeric_limits<double>::quiet_NaN(),100);
    uneven.Add(std::numeric_limits<double>::infinity(),100);
    if(!near(uneven.Fps(),10)) return 5;
    FrameRateWindow idle;
    idle.Add(5000,0);
    if(!near(idle.Fps(),0)) return 6;
    std::cout<<"Performance measurement checks passed\n";
}
