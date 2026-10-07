#!/usr/bin/env python3
"""Check the production renderer's scaled inverse, hands and stereo eye offsets."""
from pathlib import Path
import subprocess
import tempfile
import sys

root=Path(__file__).resolve().parents[3] if len(sys.argv)==1 else Path(sys.argv[1])
candidates=[root/'vendor/librw/src/vulkan/vkdevice.cpp',root/'librw/src/vulkan/vkdevice.cpp']
source=next(p for p in candidates if p.is_file()).read_text()
def function(name):
    start=source.index('\n'+name+'(')
    begin=source.index('{',start)
    depth=1; end=begin+1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}')
        end+=1
    return source[start+1:end]
code=r'''
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
using float32=float; using int32=int; using uint32=unsigned; using bool32=int;
#define nil nullptr
struct State {
 float worldToPlay[16]{},worldToPlaySubmitted[16]{},reflectionPrevViewProj[2][16]{},stereoViewProjectionUnjittered[2][16]{};
 float worldScale=1,fpHeadWorld[3]{},fpLatchedHeadPos[3]{},headPoseOffset[3]{},headPoseCorrection[3]{},eyePosition[2][3]{},eyeQuat[2][4]{};
 int modernWater=1,reflectionPrevValid=0,firstPersonActive=1,eyePosesValid=0;
} gvk;
struct SceneData { float playToWorld[16]{},reflectionReproject[2][16]{}; } scene;
SceneData* getSceneData(){return &scene;}
bool sceneReflectionsActive(){return true;}
void multiplyMatrix(float*out,const float*a,const float*b){
 float r[16]{};for(int c=0;c<4;c++)for(int row=0;row<4;row++)for(int k=0;k<4;k++)r[4*c+row]+=a[4*k+row]*b[4*c+k];memcpy(out,r,sizeof r);
}
void getFirstPersonPlayBasis(float*x,float*y,float*z){
 const float c=.5f,s=.8660254f;const float a[3]={c,0,s},b[3]={0,1,0},d[3]={-s,0,c};memcpy(x,a,12);memcpy(y,b,12);memcpy(z,d,12);
}
void check(bool ok,const char*why){if(!ok){fprintf(stderr,"FAIL %s\n",why);exit(1);}}
bool close(float a,float b){return fabsf(a-b)<.0002f;}
'''
code+='\nvoid '+function('refreshSceneReprojection')
code+='\nbool32 '+function('playPoseToFirstPersonWorld')
code+='\nbool32 '+function('firstPersonWorldPositionToPlay')
code+='\nvoid '+function('setFirstPersonEyePoses')
code+='\nbool32 '+function('getFirstPersonEyeViewFrame')
code+=r'''
int main(){
 for(float scale:{.25f,1.f,4.f}){
  gvk.worldScale=scale;
  float x[3],y[3],z[3];getFirstPersonPlayBasis(x,y,z);
  const float t[3]={3,-2,4};memset(gvk.worldToPlay,0,sizeof gvk.worldToPlay);
  for(int i=0;i<3;i++){gvk.worldToPlay[4*i]=x[i]/scale;gvk.worldToPlay[4*i+1]=y[i]/scale;gvk.worldToPlay[4*i+2]=z[i]/scale;gvk.worldToPlay[12+i]=t[i];}
  gvk.worldToPlay[15]=1;
  for(int eye=0;eye<2;eye++)for(int i=0;i<16;i++)gvk.stereoViewProjectionUnjittered[eye][i]=(i%5==0)?1:0;
  refreshSceneReprojection();float product[16];multiplyMatrix(product,gvk.worldToPlay,scene.playToWorld);
  for(int i=0;i<16;i++)check(close(product[i],i%5==0?1:0),"scaled world/play matrix inverse");
  for(int eye=0;eye<2;eye++)for(int i=0;i<16;i++)check(close(scene.reflectionReproject[eye][i],i%5==0?1:0),"unchanged-camera reflection reprojection");
  gvk.fpHeadWorld[0]=10;gvk.fpHeadWorld[1]=5;gvk.fpHeadWorld[2]=-3;
  gvk.fpLatchedHeadPos[0]=.1f;gvk.fpLatchedHeadPos[1]=1.5f;gvk.fpLatchedHeadPos[2]=-.2f;
  gvk.headPoseOffset[1]=.4f;gvk.headPoseCorrection[0]=.15f;
  const float raw[3]={.3f,1.7f,-.4f},q[4]={0,0,0,1};float wr[3],wu[3],wf[3],world[3],back[3];
  check(playPoseToFirstPersonWorld(raw,q,wr,wu,wf,world),"hand forward conversion");
  check(firstPersonWorldPositionToPlay(world,back),"hand inverse conversion");
  for(int i=0;i<3;i++)check(close(raw[i],back[i]),"hand position round trip with height and scale");
  const float eyes[2][3]={{-.032f,1.5f,0},{.032f,1.5f,0}},orient[2][4]={{0,0,0,1},{0,0,0,1}};
  setFirstPersonEyePoses(eyes,orient);float eyeWorld[2][3];
  for(int eye=0;eye<2;eye++){
   check(getFirstPersonEyeViewFrame(eye,wr,wu,wf,eyeWorld[eye]),"production eye view conversion");
   for(int axis=0;axis<3;axis++){
    float expected=gvk.fpHeadWorld[axis];
    for(int k=0;k<3;k++)expected+=scale*(k==0?x[axis]:k==1?y[axis]:z[axis])*(eyes[eye][k]+gvk.headPoseOffset[k]+gvk.headPoseCorrection[k]-gvk.fpLatchedHeadPos[k]);
    check(close(eyeWorld[eye][axis],expected),"height applied once to stereo eyes");
   }
  }
  float distance=0;for(int i=0;i<3;i++)distance+=powf(eyeWorld[0][i]-eyeWorld[1][i],2);
  check(close(sqrtf(distance),.064f*scale),"runtime metre IPD converted once to world units");
 }
 puts("PASS: production GTA renderer inverses, reflections, hands and stereo height at 25%, 100%, 400%");
}
'''
code=code.replace('#include <cstdlib>','#include <cstdlib>\n#include <initializer_list>')
with tempfile.TemporaryDirectory(prefix='gta-camera-scale-') as d:
    p=Path(d);(p/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++17','-O2',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
