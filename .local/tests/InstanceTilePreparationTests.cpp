#include "Core/Rendering/InstanceTilePreparation.h"
#include <cstdlib>
#include <vector>
using Engine::Rendering::InstanceTilePreparation;
static void require(bool value) { if (!value) std::abort(); }
int main() {
 InstanceTilePreparation cache;
 InstanceTilePreparation::Input item; item.slot=7; item.base=10; item.maximumLod=2;
 item.minimum={-1,-1,-1}; item.maximum={1,1,1}; item.ranges={{0,3}};
 auto first=cache.Prepare({item},1,0); require(first.built==1 && first.reused==0);
 auto prior=first.records[0]; require((*prior)[0].range.x==10 && (*prior)[0].origin.x==0);
 auto same=cache.Prepare({item},2,0); require(same.reused==1 && same.records[0]==prior);
 auto shadow=cache.Prepare({item},2,1); require(shadow.built==1 && shadow.records[0]!=prior);
 item.world[3].x=5; auto moved=cache.Prepare({item},3,0);
 require(moved.built==1 && (*moved.records[0])[0].origin.x==5 && (*prior)[0].origin.x==0);
 item.base=20; item.ranges={{2,4}}; auto replaced=cache.Prepare({item},4,0);
 require(replaced.built==1 && (*replaced.records[0])[0].range.x==22 && (*prior)[0].range.x==10);
 cache.Prepare({},250,0); auto reentered=cache.Prepare({item},251,0); require(reentered.built==1);
 std::vector<InstanceTilePreparation::Input> many(160,item);
 for(unsigned i=0;i<many.size();++i) many[i].slot=i+100;
 auto parallel=cache.Prepare(many,252,0); require(parallel.built==many.size());
 auto repeat=cache.Prepare(many,253,0); require(repeat.reused==many.size());
}
