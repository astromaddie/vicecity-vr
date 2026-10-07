#!/usr/bin/env python3
"""Exercise production Frame camera modifier context routing with the real SFVR helper."""
from pathlib import Path
import subprocess,tempfile,sys
root=Path(sys.argv[1]) if len(sys.argv)>1 else Path(__file__).resolve().parents[3]
base=root if (root/'src/skel/android/android.cpp').is_file() else root/'overlay'
s=(base/'src/skel/android/android.cpp').read_text();start=s.index('static void\nUpdateCameraScaleModifier(');b=s.index('{',start);depth=1;e=b+1
while depth:
 depth+=(s[e]=='{')-(s[e]=='}');e+=1
fn=s[start:e]
code=r'''
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "sfvr/sfvr_adjust.h"
#define nil nullptr
constexpr int GS_PLAYING_GAME=1;
int gGameState=1;
struct {bool m_bGameNotLoaded=false,m_bMenuActive=false,m_bWantToRestart=false,m_bWantToLoad=false;} FrontEndMenuManager;
struct CGame{static inline bool playingIntro=false;};
struct CCutsceneMgr{static bool IsRunning(){return false;}static bool IsCutsceneProcessing(){return false;}};
struct Player{bool IsPlayerInRemoteMode(){return false;}};
struct CWorld{static inline Player Players[1];static inline int PlayerInFocus=0;};
struct {bool m_WideScreenOn=false;} TheCamera;
void* FindPlayerVehicle(){return nullptr;}
namespace xrvk {bool focus=true;double now=1;bool isSessionFocused(){return focus;}long long getPredictedDisplayTimeNs(){return (long long)(now*1e9);}}
namespace androidgame {
constexpr int VR_PAD_SOURCE_LEFT_GRIP=6,VR_PAD_TARGET_NONE=0;
struct PadInput{float rightStickX=0,rightStickY=0,leftGrip=0;bool menu=false,viewActive=false,viewBack=false;};
bool preview=false,tp=true;int binding=0;float scale=1,closer=0,lower=0;
bool VrMenuConsumesInput(){return preview;}bool VrThirdPersonScreen(){return false;}bool VrThirdPersonShoulder(){return tp;}
int VrPadBinding(int){return binding;}
void VrGetCameraScaleAdjustments(float*a,float*b,float*c){*a=scale;*b=closer;*c=lower;}
void VrSetCameraScaleAdjustments(float a,float b,float c){scale=a;closer=b;lower=c;}
void VrSaveCameraScaleAdjustments(){}
}
static sfvr_adjust_state gCameraScaleAdjustState;
static sfvr_adjust_config gCameraScaleAdjustConfig;
static bool gCameraScaleAdjustInitialized,gCameraScaleAdjustWasActive,gCameraScaleAdjustDirty;
'''+fn+r'''
void check(bool b,const char*m){if(!b){fprintf(stderr,"FAIL %s\n",m);exit(1);}}
void reset(){gCameraScaleAdjustInitialized=gCameraScaleAdjustWasActive=gCameraScaleAdjustDirty=false;gCameraScaleAdjustState={};androidgame::scale=1;androidgame::closer=androidgame::lower=0;xrvk::now=1;xrvk::focus=true;androidgame::preview=false;androidgame::tp=true;androidgame::binding=0;}
androidgame::PadInput step(float x,float y,bool grip){androidgame::PadInput p;p.rightStickX=x;p.rightStickY=y;p.leftGrip=grip?1:0;xrvk::now+=1./90;UpdateCameraScaleModifier(&p);return p;}
int main(){
 reset();step(1,1,true);auto p=step(1,1,true);check(androidgame::scale<1&&androidgame::closer<0&&androidgame::lower<0,"third-person live adjustment");check(p.rightStickX==0&&p.rightStickY==0,"consumed stick");
 reset();androidgame::binding=1;step(1,1,true);p=step(1,1,true);check(androidgame::scale==1&&p.rightStickX==1,"custom grip retains gameplay binding");
 reset();androidgame::tp=false;androidgame::preview=true;step(1,1,true);p=step(1,1,true);check(androidgame::scale<1&&androidgame::closer==0&&androidgame::lower==0,"first-person preview is scale only");
 reset();xrvk::focus=false;step(1,1,true);p=step(1,1,true);check(androidgame::scale==1,"focus loss cannot adjust saved settings");
 reset();androidgame::PadInput menu;menu.menu=true;menu.leftGrip=1;menu.rightStickX=1;UpdateCameraScaleModifier(&menu);check(androidgame::scale==1,"Menu priority");
 puts("PASS: production GTA modifier routes third person, custom grip, FP preview, focus and Menu");
}
'''
with tempfile.TemporaryDirectory(prefix='gta-camera-modifier-') as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 include=base/'third_party/sfvr/include';impl=base/'third_party/sfvr/src/sfvr_adjust.c'
 subprocess.run(['cc','-std=c99','-I'+str(include),'-c',str(impl),'-o',str(p/'adjust.o')],check=True)
 subprocess.run(['c++','-std=c++17','-I'+str(include),str(p/'test.cpp'),str(p/'adjust.o'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
