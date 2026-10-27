// Anomaly Coop. Original NPC sequences: state_mgr_animstate_list / state_mgr_animation_list.
// Attachment offsets and meshes come from the installed guitar_a / harmonica_a configuration.
// No third-party co-op implementation or assets are used.
#include "stdafx.h"
#include "coop_instrument.h"
#include "coop_manager.h"
#include "coop_survival.h"
#include "coop_world.h"
#include "Actor.h"
#include "Level.h"
#include "Inventory.h"
#include "inventory_item.h"
#include "attachable_item.h"
#include "InventoryOwner.h"
#include "CharacterPhysicsSupport.h"
#include "PHMovementControl.h"
#include "../Include/xrRender/Kinematics.h"
#include "../Include/xrRender/KinematicsAnimated.h"
#include "../xrSound/Sound.h"
#include "lua.hpp"
#include <atomic>
#include <cmath>

namespace coop { namespace players { namespace {
instrument::State g_own;
u64 g_own_at = 0;
u32 g_serial = 0;
std::atomic<const CActor*> g_held{nullptr};
bool g_twist_off = false;
struct Presentation
{
    const CGameObject* owner = nullptr;
    const IRenderVisual* skeleton = nullptr;
    instrument::State state;
    u64 touched = 0;
    ref_sound song;
    u32 song_serial = 0;
    u8 song_track = 0, song_kind = 0;
    IRenderVisual* prop = nullptr;
    u8 prop_kind = 0, npc_kind = 0;
    bool show_prop = false, missing_logged = false;
};
// Heap-owned: release sound/render resources while the world still exists, never at process-static teardown.
xr_map<u16, Presentation*> g_present;

CActor* own_actor()
{
    return world_up() ? smart_cast<CActor*>(Level().CurrentEntity()) : nullptr;
}
bool alive(CGameObject* obj)
{
    CEntityAlive* a = obj ? obj->cast_entity_alive() : nullptr;
    return a && a->g_Alive() && survival::life_of(obj->ID()) == survival::Life::Alive;
}
void own_off(CActor* a)
{
    if (!a || g_held.load() != a) return;
    g_held.store(nullptr);
    if (g_twist_off) a->SetCallbacks();
    g_twist_off = false;
    a->ResetAnimationCache();
}
void dispose(Presentation*& p)
{
    if (!p) return;
    p->song.stop(); p->song.destroy();
    if (p->prop) ::Render->model_Delete(p->prop);
    xr_delete(p);
}
Presentation& view(CGameObject* obj)
{
    Presentation*& p = g_present[obj->ID()];
    if (p && p->owner != obj) dispose(p);
    if (!p) { p=xr_new<Presentation>(); p->owner=obj; }
    p->touched=mgr().now_ms();
    return *p;
}
const char* section(u8 kind) { return kind == 1 ? "guitar_a" : "harmonica_a"; }
bool resource(const char* alias, const char* name, const char* ext)
{
    string_path path;
    return FS.exist(path, alias, name, ext);
}
// Safely resolve the full ORIGINAL sequence on this visual. A missing sequence leaves normal animation running.
bool pose_motion(IKinematicsAnimated* ka, const instrument::State& s, MotionID& motion, float& phase, bool& prop)
{
    const bool guitar=s.kind==1;
    const char* enter_g[] = {"idle_0_to_sit_1", "sit_1_guitar_0_0", "sit_1_guitar_0_1"};
    const char* enter_h[] = {"idle_0_to_sit_2", "sit_2_harmonica_1_0", "sit_2_harmonica_1_1"};
    const char* leave_g[] = {"guitar_0_sit_1_0", "guitar_0_sit_1_1", "sit_1_to_idle_0"};
    const char* leave_h[] = {"harmonica_1_sit_2_0", "harmonica_1_sit_2_1", "sit_2_to_idle_0"};
    const char** enter=guitar ? enter_g : enter_h;
    const char** leave=guitar ? leave_g : leave_h;
    MotionID in[3], out[3];
    const MotionID loop=ka->ID_Cycle_Safe(guitar ? "guitar_0" : "harmonica_0");
    if (!loop.valid()) return false;
    for (u32 i=0; i<3; ++i)
    {
        in[i]=ka->ID_Cycle_Safe(enter[i]); out[i]=ka->ID_Cycle_Safe(leave[i]);
        if (!in[i].valid() || !out[i].valid()) return false;
    }
    phase=float(s.phase==3 ? s.phase_ms : s.age_ms)/1000.f;
    MotionID* seq=s.phase==3 ? out : in;
    for (u32 i=0; i<3; ++i)
    {
        const float length=ka->get_animation_length(seq[i]);
        if (!std::isfinite(length) || length<=0.f) return false;
        if (phase<length)
        {
            motion=seq[i];
            prop=s.phase==3 ? i==0 : i>=1;
            return true;
        }
        phase-=length;
    }
    if (s.phase==3) return false;
    motion=loop; prop=true;
    const float length=ka->get_animation_length(loop);
    if (!std::isfinite(length) || length<=0.f) return false;
    phase=fmodf(phase, length);
    return true;
}
void update_sound(Presentation& p, CGameObject* obj, const instrument::State& s)
{
    if (s.phase!=2 || !s.kind || !alive(obj))
    {
        p.song.stop(); p.song.destroy(); p.song_serial=0; return;
    }
    const bool same=p.song_serial==s.serial && p.song_track==s.track && p.song_kind==s.kind;
    if (!same)
    {
        p.song.stop(); p.song.destroy();
        p.song_serial=s.serial; p.song_track=s.track; p.song_kind=s.kind;
        string64 path;
        xr_sprintf(path, "music\\%s_%u", s.kind==1 ? "guitar" : "harmonica", unsigned(s.track));
        if (resource("$game_sounds$", path, ".ogg"))
        {
            p.song.create(path, st_Effect, 0);
            p.song.play_at_pos(nullptr, obj->Position()); // no AI sound event or dangling g_object
            if (p.song._feedback())
            {
                if (p.song._handle()->channels_num()==1) p.song._feedback()->switch_to_3D();
                p.song.set_range(1.f, 45.f);
                p.song._feedback()->set_start_position(float(s.phase_ms)/1000.f);
            }
        }
    }
    if (p.song._feedback())
    {
        p.song.set_position(obj->Position());
        // OpenAL stereo buffers ignore 3D distance. Bound those music assets explicitly as well.
        const float d=Device.vCameraPosition.distance_to(obj->Position());
        p.song.set_volume(d>=45.f ? 0.f : clampr((45.f-d)/44.f,0.f,1.f));
    }
}
} // namespace

int lua_instrument(lua_State* L)
{
    int v[4]={};
    for (int i=0; i<4; ++i)
    {
        if (lua_type(L,i+1)!=LUA_TNUMBER) { lua_pushboolean(L,0); return 1; }
        const double n=lua_tonumber(L,i+1);
        if (!std::isfinite(n) || n<0 || n>instrument::kMaxSongMs || floor(n)!=n)
        { lua_pushboolean(L,0); return 1; }
        v[i]=int(n);
    }
    CActor* a=own_actor();
    if (!mgr().active() || !a) { lua_pushboolean(L,0); return 1; }
    if (v[1]==0)
    {
        g_own=instrument::State(); own_off(a); instrument_forget(a->ID()); lua_pushboolean(L,1); return 1;
    }
    if (!alive(a) || a->Holder()) { lua_pushboolean(L,0); return 1; }
    Fvector velocity; velocity.set(0.f,0.f,0.f);
    if (a->character_physics_support() && a->character_physics_support()->movement()) velocity=a->character_physics_support()->movement()->GetVelocity();
    if (!instrument::stationary(a->MovingState(),velocity.x,velocity.y,velocity.z,true)) { lua_pushboolean(L,0); return 1; }
    instrument::State s;
    s.kind=u8(v[0]); s.phase=u8(v[1]); s.track=u8(v[2]); s.duration_ms=u32(v[3]);
    if (v[0]>2 || v[1]>3 || v[2]>28 || !v[0]) { lua_pushboolean(L,0); return 1; }
    const u64 now=mgr().now_ms();
    if (g_own.kind==s.kind && (s.phase==3 || (s.phase==2 && g_own.phase==1)))
    { s.serial=g_own.serial; s.age_ms=g_own.age_ms+u32(now-g_own_at); }
    else { if (!++g_serial) ++g_serial; s.serial=g_serial; }
    if (!s.valid()) { lua_pushboolean(L,0); return 1; }
    IKinematicsAnimated* ka=a->Visual() ? a->Visual()->dcast_PKinematicsAnimated() : nullptr;
    MotionID probe; float position=0.f; bool attached=false;
    const char* sect=section(s.kind);
    if (!ka || !pose_motion(ka,s,probe,position,attached) || !pSettings->section_exist(sect) ||
        !pSettings->line_exist(sect,"visual") || !pSettings->line_exist(sect,"attach_bone_name") ||
        !pSettings->line_exist(sect,"attach_angle_offset") || !pSettings->line_exist(sect,"attach_position_offset") ||
        !resource("$game_meshes$",pSettings->r_string(sect,"visual"),nullptr) ||
        ka->dcast_PKinematics()->LL_BoneID(pSettings->r_string(sect,"attach_bone_name"))==BI_NONE)
    { lua_pushboolean(L,0); return 1; }
    const Fvector angle_offset=pSettings->r_fvector3(sect,"attach_angle_offset");
    const Fvector position_offset=pSettings->r_fvector3(sect,"attach_position_offset");
    if (!instrument::finite3(angle_offset.x,angle_offset.y,angle_offset.z) ||
        !instrument::finite3(position_offset.x,position_offset.y,position_offset.z))
    { lua_pushboolean(L,0); return 1; }
    if (s.phase==2)
    {
        string64 path; xr_sprintf(path,"music\\%s_%u",s.kind==1 ? "guitar" : "harmonica",unsigned(s.track));
        if (!resource("$game_sounds$",path,".ogg")) { lua_pushboolean(L,0); return 1; }
    }
    g_own=s; g_own_at=now;
    lua_pushboolean(L,1); return 1;
}
instrument::State capture_instrument() { return g_own; }
bool own_instrument_held(const CActor* a) { return a && g_held.load()==a; }

bool instrument_pose(CGameObject* obj, const instrument::State& s)
{
    auto found=g_present.find(obj->ID());
    if (!s.kind)
    {
        if (found!=g_present.end()) { found->second->show_prop=false; update_sound(*found->second,obj,s); }
        return false;
    }
    Presentation& p=view(obj);
    p.state=s;
    update_sound(p,obj,s);
    IKinematicsAnimated* ka=obj->Visual() ? obj->Visual()->dcast_PKinematicsAnimated() : nullptr;
    MotionID motion; float phase=0.f; bool prop=false;
    if (!ka || !pose_motion(ka,s,motion,phase,prop))
    {
        p.show_prop=false;
        if (s.phase!=3 && !p.missing_logged) { ::Msg("! [coop] instrument: visual %s has no complete original NPC sequence", obj->cNameVisual().c_str()); p.missing_logged=true; }
        return false;
    }
    p.skeleton=obj->Visual(); p.show_prop=prop;
    // Explicit partitions: global NPC script cycles may have bone_or_part=BI_NONE.
    for (u16 part=0; part<4; ++part)
    {
        CBlend* playing=nullptr;
        for (u32 i=0,n=ka->LL_PartBlendsCount(part); i<n; ++i)
        {
            CBlend* b=ka->LL_PartBlend(part,i);
            if (b && b->motionID==motion && b->blend_state()==CBlend::eAccrue) { playing=b; break; }
        }
        if (!playing) playing=ka->LL_PlayCycle(part,motion,TRUE,nullptr,nullptr);
        if (playing)
        {
            playing->timeCurrent=clampr(phase*ka->LL_GetMotionDef(motion)->Speed(),0.f,_max(0.f,playing->timeTotal-.001f));
            playing->speed=0.f;
        }
    }
    return true;
}
void instrument_frame()
{
    const u64 now=mgr().now_ms();
    CActor* a=own_actor();
    if (a && g_own.kind)
    {
        Fvector vel; vel.set(0.f,0.f,0.f);
        if (a->character_physics_support() && a->character_physics_support()->movement()) vel=a->character_physics_support()->movement()->GetVelocity();
        if (!instrument::stationary(a->MovingState(),vel.x,vel.y,vel.z,alive(a)) || a->Holder()) g_own=instrument::State();
        else instrument::advance(g_own,u32(now-g_own_at));
        g_own_at=now;
    }
    const bool held=a && !survival::own_pose_held(a) && instrument_pose(a,g_own);
    if (held)
    {
        if (g_held.load()!=a)
        {
            g_held.store(a);
            a->CStepManager::on_animation_start(MotionID(),nullptr);
            if (!a->Holder()) { a->ResetCallbacks(); g_twist_off=true; }
        }
    }
    else own_off(a);
    for (auto it=g_present.begin(); it!=g_present.end();)
    {
        CGameObject* obj=g_pGameLevel ? smart_cast<CGameObject*>(Level().Objects.net_Find(it->first)) : nullptr;
        Presentation* p=it->second;
        if (!obj || obj!=p->owner || !alive(obj) || now-p->touched>3000)
        { dispose(it->second); it=g_present.erase(it); }
        else ++it;
    }
}
void instrument_forget(u16 id)
{
    auto it=g_present.find(id);
    if (it!=g_present.end()) { dispose(it->second); g_present.erase(it); }
}
void instrument_reset()
{
    CActor* a=own_actor(); own_off(a); g_held.store(nullptr); g_twist_off=false;
    g_own=instrument::State(); g_own_at=0;
    for (auto& it:g_present) dispose(it.second);
    g_present.clear();
}
void instrument_npc_attachment(CGameObject* obj, u8 kind)
{
    auto it=g_present.find(obj->ID());
    if (!kind && it==g_present.end()) return;
    Presentation& p=view(obj); p.npc_kind=kind; p.show_prop=kind!=0;
    if (CInventoryOwner* owner=obj->cast_inventory_owner())
        for (PIItem item:owner->inventory().m_all)
        {
            const bool guitar=item->object().cNameSect()=="guitar_a", harmonica=item->object().cNameSect()=="harmonica_a";
            if (!guitar && !harmonica) continue;
            if (CAttachableItem* a=smart_cast<CAttachableItem*>(item))
            {
                const bool on=kind==(guitar ? 1 : 2);
                if (a->enabled()!=on) a->enable(on);
                if (on) p.show_prop=false; // the real attachment is present: no second model
            }
        }
}
void instrument_render(CGameObject* obj, IDSGraphManager* DM)
{
    auto it=g_present.find(obj->ID());
    if (it==g_present.end() || it->second->owner!=obj || !alive(obj)) return;
    Presentation& p=*it->second;
    if (!p.show_prop) return;
    const u8 kind=p.npc_kind ? p.npc_kind : p.state.kind;
    if (!kind) return;
    IKinematics* k=obj->Visual() ? obj->Visual()->dcast_PKinematics() : nullptr;
    const char* sect=section(kind);
    if (!k || !pSettings->section_exist(sect) || !pSettings->line_exist(sect,"attach_bone_name") ||
        !pSettings->line_exist(sect,"attach_angle_offset") || !pSettings->line_exist(sect,"attach_position_offset")) return;
    const u16 bone=k->LL_BoneID(pSettings->r_string(sect,"attach_bone_name"));
    if (bone==BI_NONE) return;
    const Fvector angle_offset=pSettings->r_fvector3(sect,"attach_angle_offset");
    const Fvector position_offset=pSettings->r_fvector3(sect,"attach_position_offset");
    if (!instrument::finite3(angle_offset.x,angle_offset.y,angle_offset.z) ||
        !instrument::finite3(position_offset.x,position_offset.y,position_offset.z)) return;
    if (p.prop_kind!=kind)
    {
        if (p.prop) ::Render->model_Delete(p.prop);
        p.prop_kind=kind;
        if (!pSettings->line_exist(sect,"visual")) return;
        const char* model=pSettings->r_string(sect,"visual");
        if (resource("$game_meshes$",model,nullptr)) p.prop=::Render->model_Create(model);
    }
    if (!p.prop) return;
    Fmatrix offset, mat;
    offset.setHPB(angle_offset.x,angle_offset.y,angle_offset.z);
    offset.c=position_offset;
    k->CalculateBones(TRUE);
    mat.mul_43(obj->XFORM(),k->LL_GetTransform(bone)); mat.mulB_43(offset);
    if (IKinematics* pk=p.prop->dcast_PKinematics()) pk->CalculateBones(TRUE);
    DM->add_Dynamic(p.prop,&mat);
}
} }
