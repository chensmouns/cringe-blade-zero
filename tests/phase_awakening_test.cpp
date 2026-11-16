// Exercise the actual game-thread lease with a fake world / reflected-call boundary.
// No game process, assets, injected input, audio device or engine addresses are used.
#include <windows.h>
#include <algorithm>
#include <any>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
namespace ue {
struct Object { bool alive=true; std::map<std::string,std::any> values; unsigned char memory[128]{}; };
struct Array {void* data=nullptr;int32_t num=0,max=0;};
Object dummy;
std::array<unsigned char,0x126ed00> image{};
uintptr_t Base(){return reinterpret_cast<uintptr_t>(image.data());}
bool Alive(Object* o){return o&&o->alive;}
Object* Default(const char*){return &dummy;}
Object* FindObject(const char*){return &dummy;}
bool IsA(Object*,Object*){return false;}
bool AddReference(Object*){return true;}
bool ReleaseReference(Object*){return true;}
int32_t StructSize(Object*){return 16;}
template<class T>T& At(Object* o,int32_t offset){return *reinterpret_cast<T*>(o->memory+offset);}
}
using ue::Object;using ue::Array;
struct Identity {Object* object=nullptr;};
Identity Id(Object* o){return {o};}
bool Same(Identity id){return ue::Alive(id.object);}
template<class T>T Property(Object* o,const char* key){if(!o)return {};const auto it=o->values.find(key);return it==o->values.end()?T{}:std::any_cast<T>(it->second);}
Object* Class(const char*){return &ue::dummy;}
Object* LoadClassPackage(const char*){return &ue::dummy;}
int32_t Offset(Object*,const char*){return 0;}
bool FaceReadable(const void*,size_t){return true;}
std::string Text(Object*){return "fake";}
void Logf(const char*,...){ }
void Status(const wchar_t*,...){ }
struct Context {Object* pawn=nullptr;Object* world=nullptr;};
Context context_;
bool context_ready_=true,dead_=false;
bool ContextNow(Context& c){c=context_;return context_ready_;}
Object* music_=nullptr;
int removes_=0,clears_=0,updates_=0,released_=0;
bool cleared_story_=false;
struct Call {
    Object* object;std::string function;std::map<std::string,std::any> values;
    Call(Object* o,const char* f):object(o),function(f){}
    template<class T>Call& Set(const char* name,const T& value){values[name]=value;return *this;}
    bool Invoke(){
        if(function=="RemoveActiveGameplayEffect")++removes_;
        if(function=="ClearMusic"){++clears_;cleared_story_=std::any_cast<bool>(values["bClearStoryMusic"]);}
        if(function=="UpdateGameplayMusic")++updates_;
        return object!=nullptr;
    }
    template<class T>T Get(const char* name){
        if constexpr(std::is_same_v<T,bool>)return function=="IsDead"?dead_:true;
        if constexpr(std::is_same_v<T,Object*>)return function=="GetWorldSubsystem"?music_:nullptr;
        (void)name;return T{};
    }
};
namespace NpcCombo {int ClampAwakening(int ms){return std::clamp(ms,1000,600000);}}
#include "phase_awakening.inl"
int failures_=0;
void Check(bool result,const char* what){std::printf("%s %s\n",result?"PASS":"FAIL",what);if(!result)++failures_;}
int main(){
    Object pawn,world,asc,music,event,foreign;
    context_={&pawn,&world};music_=&music;
    const auto arm=[&](int current_id,Object* current_event){
        awakening_={};awakening_.pawn=Id(&pawn);awakening_.world=Id(&world);awakening_.asc=Id(&asc);
        awakening_.music=Id(&music);awakening_.event=Id(&event);awakening_.music_id=42;
        awakening_.until=GetTickCount64()+10000;awakening_.effects.push_back({123,true,{}});
        music.values["CurPlayingID_Gameplay"]=int32_t(current_id);music.values["CurPlayingEvent_Gameplay"]=current_event;
        removes_=clears_=updates_=0;cleared_story_=false;dead_=false;context_ready_=true;
    };
    arm(42,&event);EndAwakening("test-expiry");
    Check(removes_==1&&clears_==1&&updates_==1&&!cleared_story_&&!awakening_.until,"expiry removes the owned buff and restores gameplay music only");
    EndAwakening("again");Check(removes_==1&&clears_==1,"cleanup is idempotent");
    arm(99,&event);EndAwakening("new-id");Check(removes_==1&&clears_==0,"a newer playing ID is not stopped");
    arm(42,&foreign);EndAwakening("new-event");Check(removes_==1&&clears_==0,"a different event is not stopped even with the same ID");
    arm(42,&event);EndAwakening("recast",false);Check(removes_==1&&clears_==0,"recast can replace its buff without interrupting its music");
    arm(42,&event);awakening_.until=GetTickCount64()-1;DriveAwakening();Check(!awakening_.until&&clears_==1,"timer expires independently of any active combo");
    arm(42,&event);dead_=true;DriveAwakening();Check(!awakening_.until&&removes_==1,"death clears the lease");
    arm(42,&event);context_.pawn=&foreign;DriveAwakening();Check(!awakening_.until,"a replaced player clears the old lease");context_.pawn=&pawn;
    arm(42,&event);world.alive=false;DriveAwakening();Check(!awakening_.until&&clears_==0,"an expired world is never called for audio cleanup");world.alive=true;
    arm(42,&event);awakening_.music_id=0;awakening_.before_id=12;awakening_.capture_until=GetTickCount64()+1000;DriveAwakening();
    Check(awakening_.music_id==42,"the expected awakening event is captured after activation");
    arm(99,&foreign);awakening_.music_id=0;awakening_.before_id=12;awakening_.capture_until=GetTickCount64()+1000;DriveAwakening();
    Check(awakening_.music_id==0,"capture never claims an unrelated music event");
    arm(42,&event);awakening_.music_id=0;awakening_.before_id=42;awakening_.capture_until=GetTickCount64()+1000;DriveAwakening();
    Check(awakening_.music_id==0,"an unchanged preexisting track is not claimed");
    return failures_?1:0;
}
