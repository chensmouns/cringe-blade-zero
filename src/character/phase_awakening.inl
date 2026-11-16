// Included inside npc_retarget.cpp's game-thread runtime namespace. Each application uses
// a private effect template and keeps the returned active handle; no shared asset is edited.
struct AwakeningEffectHandle { int32_t handle=-1; bool applied=false; unsigned char pad[3]{}; };
static_assert(sizeof(AwakeningEffectHandle)==8);
struct AwakeningSpec { void* data=nullptr; void* control=nullptr; };
static_assert(sizeof(AwakeningSpec)==16);
struct PhaseAwakening {
    Identity pawn,world,asc,music,event;
    int32_t music_id=0,before_id=0;
    ULONGLONG until=0,capture_until=0,next_check=0;
    std::vector<AwakeningEffectHandle> effects;
    std::vector<Identity> templates;
} awakening_;

Object* AwakeningMusic(Object* pawn){
    Call get(ue::Default("/Script/Engine.Default__SubsystemBlueprintLibrary"),"GetWorldSubsystem");
    return get.Set("ContextObject",pawn).Set("Class",Class("/Script/ProjectPBZ.MusicSubsystem")).Invoke()?get.Get<Object*>("ReturnValue"):nullptr;
}
// The same shared-reference release called by this build's execMakeSpecHandle / execSetDuration.
// Pin its bytes before making a spec. Never guess a reference-controller vtable layout.
bool AwakeningSpecReleaseReady(){
    static constexpr unsigned char code[]={0x48,0x85,0xc9,0x74,0x42,0x53,0x48,0x83,0xec,0x20,0x48,0x89,0x7c,0x24,0x30,0x48};
    return !std::memcmp(reinterpret_cast<const void*>(ue::Base()+0x126ec80),code,sizeof(code));
}
void ReleaseAwakeningSpec(AwakeningSpec spec){
    if(spec.control&&AwakeningSpecReleaseReady())reinterpret_cast<void(__fastcall*)(void*)>(ue::Base()+0x126ec80)(spec.control);
}
void EndAwakening(const char* why,bool restore_music=true){
    if(!awakening_.until)return;
    size_t removed=0;
    if(Same(awakening_.asc)) for(const auto& handle:awakening_.effects){
        Call remove(awakening_.asc.object,"RemoveActiveGameplayEffect");
        if(remove.Set("Handle",handle).Set("StacksToRemove",int32_t{-1}).Invoke()&&remove.Get<bool>("ReturnValue"))++removed;
    }
    bool restored=false;
    if(restore_music&&Same(awakening_.world)&&Same(awakening_.music)&&awakening_.music_id!=0&&Same(awakening_.event)&&
       Property<int32_t>(awakening_.music.object,"CurPlayingID_Gameplay")==awakening_.music_id&&
       Property<Object*>(awakening_.music.object,"CurPlayingEvent_Gameplay")==awakening_.event.object){
        Call clear(awakening_.music.object,"ClearMusic");
        if(clear.Set("FadeOutTime",0.15f).Set("bClearGameplayMusic",true).Set("bClearStoryMusic",false).Invoke()){
            restored=Call(awakening_.music.object,"UpdateGameplayMusic").Set("bIsBattleEnd",false).Invoke();
        }
    }
    for(const Identity& t:awakening_.templates)if(Same(t))ue::ReleaseReference(t.object);
    Logf("NPC_AWAKEN end reason=%s buffsRemoved=%zu/%zu musicRestored=%d ownedPlayingID=%d",why,removed,awakening_.effects.size(),restored?1:0,awakening_.music_id);
    awakening_={};
}
Object* AwakeningTemplate(const char* package,Object* pawn){
    Object* cls=LoadClassPackage(package);
    if(!cls)return nullptr;
    const std::string p(package),leaf=p.substr(p.find_last_of('/')+1);
    Object* cdo=ue::Default((p+".Default__"+leaf+"_C").c_str());
    if(!ue::Alive(cdo))return nullptr;
    Call clone(ue::Default("/Script/SGameAI.Default__SGameAIFunctionLibrary"),"DuplicateObject");
    if(!clone.Set("SourceObject",cdo).Set("Outer",pawn).Invoke())return nullptr;
    Object* copy=clone.Get<Object*>("ReturnValue");
    if(!ue::Alive(copy)||copy==cdo||!ue::AddReference(copy))return nullptr;
    awakening_.templates.push_back(Id(copy));
    const int32_t stacking=Offset(copy,"StackingType"),policy=Offset(copy,"DurationPolicy"),components=Offset(copy,"GEComponents");
    if(stacking<0||policy<0||components<0)return nullptr;
    ue::At<uint8_t>(copy,stacking)=0; // independent handles, including when the original weapon is equipped
    ue::At<uint8_t>(copy,policy)=2; // HasDuration; the spec also expires natively if our tick is interrupted
    Array& list=ue::At<Array>(copy,components);
    if(list.num<0||list.num>128||list.max<list.num||(list.num&&!FaceReadable(list.data,static_cast<size_t>(list.num)*sizeof(Object*))))return nullptr;
    // The old mod replaces Tang's out-of-combat/weapon requirements with the awakening lifetime.
    // Retain its native modifiers, tags, weapon-buff component and gameplay cues.
    Object* tags=Class("/Script/GameplayAbilities.TargetTagRequirementsGameplayEffectComponent");
    Object* duration=Class("/Script/SGameCombatSystem.TagRequirementsDurationGameplayEffectComponent");
    int32_t keep=0;auto** items=static_cast<Object**>(list.data);
    for(int32_t i=0;i<list.num;++i)if(!ue::IsA(items[i],tags)&&!ue::IsA(items[i],duration))items[keep++]=items[i];
    for(int32_t i=keep;i<list.num;++i)items[i]=nullptr;
    list.num=keep;
    return copy;
}
bool ApplyAwakeningEffect(Object* effect,Object* pawn,Object* asc){
    if(!effect||!AwakeningSpecReleaseReady())return false;
    if(ue::StructSize(Class("/Script/GameplayAbilities.GameplayEffectSpecHandle"))!=sizeof(AwakeningSpec)||
       ue::StructSize(Class("/Script/GameplayAbilities.ActiveGameplayEffectHandle"))!=sizeof(AwakeningEffectHandle))return false;
    Object* library=ue::Default("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary");
    Call make(library,"MakeSpecHandle");
    if(!make.Set("InGameplayEffect",effect).Set("InInstigator",pawn).Set("InEffectCauser",pawn).Set("InLevel",1.0f).Invoke())return false;
    const AwakeningSpec spec=make.Get<AwakeningSpec>("ReturnValue");
    if(!spec.data)return false;
    Call duration(library,"SetDuration");
    const ULONGLONG now=GetTickCount64();
    const float seconds=now<awakening_.until?static_cast<float>(awakening_.until-now)/1000.0f:0.001f;
    if(!duration.Set("SpecHandle",spec).Set("duration",(std::max)(seconds,0.001f)).Invoke()){
        ReleaseAwakeningSpec(spec);return false;
    }
    const AwakeningSpec timed=duration.Get<AwakeningSpec>("ReturnValue");
    if(!timed.data){ReleaseAwakeningSpec(spec);return false;}
    Call apply(asc,"BP_ApplyGameplayEffectSpecToSelf");
    const bool invoked=apply.Set("SpecHandle",timed).Invoke();
    const AwakeningEffectHandle handle=invoked?apply.Get<AwakeningEffectHandle>("ReturnValue"):AwakeningEffectHandle{};
    ReleaseAwakeningSpec(timed);ReleaseAwakeningSpec(spec);
    if(handle.handle<0)return false;
    awakening_.effects.push_back(handle);
    Logf("NPC_AWAKEN applied effect=%s handle=%d",Text(effect).c_str(),handle.handle);
    return true;
}
void StartAwakening(const Context& context,Object* asc,int duration_ms,int32_t before_id){
    const int32_t old_music=awakening_.music_id;
    const Identity old_event=awakening_.event,old_subsystem=awakening_.music;
    const bool renewed=Same(awakening_.pawn)&&awakening_.pawn.object==context.pawn;
    EndAwakening("recast",!renewed);
    awakening_.pawn=Id(context.pawn);awakening_.world=Id(context.world);awakening_.asc=Id(asc);
    const ULONGLONG now=GetTickCount64();awakening_.until=now+static_cast<ULONGLONG>(NpcCombo::ClampAwakening(duration_ms));
    awakening_.capture_until=now+5000;awakening_.before_id=before_id;
    awakening_.music=Id(AwakeningMusic(context.pawn));
    Object* action=ue::FindObject("/PBZCombatAsset/ActorData/Mobs/067_MoTian/Ability/GA_MoTian_SwitchPhase.GA_MoTian_SwitchPhase_C:Main.GAA_MusicChange_C_0");
    awakening_.event=Id(Property<Object*>(action,"Music Event"));
    if(renewed&&Same(old_event)&&Same(old_subsystem)&&old_event.object==awakening_.event.object&&old_subsystem.object==awakening_.music.object)
        awakening_.music_id=old_music;
    const bool body=ApplyAwakeningEffect(AwakeningTemplate("/PBZCombatAsset/ActorData/Players/Hun/Weapon/HeiMoShang/GE_Hun_HeiMoShang_Buff",context.pawn),context.pawn,asc);
    const bool blade=ApplyAwakeningEffect(AwakeningTemplate("/PBZCombatAsset/ActorData/Players/Hun/Weapon/Tang/UpgradeData/GE_Weapon_Tang_Buff_Lv1",context.pawn),context.pawn,asc);
    Logf("NPC_AWAKEN start durationMs=%d body=%d blade=%d expectedMusic=%s",NpcCombo::ClampAwakening(duration_ms),body?1:0,blade?1:0,Text(awakening_.event.object).c_str());
    if(!body||!blade)Status(L"觉醒增益未完整加载；音乐仍会按时恢复，请查看日志");
}
void DriveAwakening(){
    if(!awakening_.until)return;
    const ULONGLONG now=GetTickCount64();
    if(now<awakening_.next_check)return;
    awakening_.next_check=now+100;
    if(!Same(awakening_.pawn)||!Same(awakening_.world)||!Same(awakening_.asc)){EndAwakening("owner-gone");return;}
    Context context;
    if(ContextNow(context)&&(context.world!=awakening_.world.object||context.pawn!=awakening_.pawn.object)){EndAwakening("player-or-world-changed");return;}
    if(now<=awakening_.capture_until&&Same(awakening_.music)&&Same(awakening_.event)){
        Object* music=awakening_.music.object;
        const int32_t id=Property<int32_t>(music,"CurPlayingID_Gameplay");
        if(id!=0&&id!=awakening_.before_id&&Property<Object*>(music,"CurPlayingEvent_Gameplay")==awakening_.event.object&&id!=awakening_.music_id){
            awakening_.music_id=id;Logf("NPC_AWAKEN music captured playingID=%d",id);
        }
    }
    Call dead(ue::Default("/Script/SGameAI.Default__SGameAIStateUtils"),"IsDead");
    if(now>=awakening_.until)EndAwakening("duration-ended");
    else if(dead.Set("Pawn",awakening_.pawn.object).Invoke()&&dead.Get<bool>("ReturnValue"))EndAwakening("player-dead");
}
