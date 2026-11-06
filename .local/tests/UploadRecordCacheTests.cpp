#include "Core/Rendering/UploadRecordCache.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
using Engine::Rendering::UploadRecordCache;
void Check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main() {
    try {
        using Record=std::array<uint32_t,8>;
        UploadRecordCache first,second;
        std::array<Record,32> upload{};
        first.Prepare(upload.size(),sizeof(Record)); second.Prepare(upload.size(),sizeof(Record));
        Record zero{};
        Check(first.Write(0,zero.data(),upload.data()),"zero-valued first record was skipped");
        // An unchanged record must not touch mapped memory at all. A null
        // destination makes an accidental read or write fail deterministically.
        Check(!first.Write(0,zero.data(),nullptr),"unchanged record was rewritten");
        Check(second.Write(0,zero.data(),upload.data()),"rotated GPU buffer reused another buffer's validity");
        Record edited{}; edited[4]=123;
        Check(first.Write(0,edited.data(),upload.data()) && upload[0]==edited,"edited payload lost");
        first.Prepare(upload.size(),sizeof(Record));
        Check(!first.Write(0,edited.data(),nullptr),"unchanged capacity reset a live mirror");
        first.Invalidate(0); upload[0]=zero;
        Check(first.Write(0,edited.data(),upload.data()) && upload[0]==edited,"external overwrite was not repaired");
        first.Clear(); first.Prepare(upload.size(),sizeof(Record));
        Check(first.Write(0,edited.data(),upload.data()),"replaced buffer retained stale validity");
        std::vector<std::thread> workers;
        for(size_t worker=0;worker<4;++worker) workers.emplace_back([&,worker] {
            for(size_t slot=worker;slot<upload.size();slot+=4) {
                Record value{}; value.fill(uint32_t(slot+10));
                first.Write(slot,value.data(),upload.data());
            }
        });
        for(auto& worker:workers) worker.join();
        for(size_t slot=0;slot<upload.size();++slot) {
            Record expected{}; expected.fill(uint32_t(slot+10));
            Check(upload[slot]==expected,"parallel record publication corrupted a neighbor");
            Check(!first.Write(slot,expected.data(),nullptr),"parallel mirror was not retained");
        }
        first.Prepare(2,sizeof(uint32_t)); uint32_t value=77; std::array<uint32_t,2> resized{};
        Check(first.Write(1,&value,resized.data()) && resized[1]==value,"stride/capacity change retained stale bytes");
        std::cout<<"Upload record initialization, CPU-only comparison, frame rotation, external writes, replacement and parallel writes passed.\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
