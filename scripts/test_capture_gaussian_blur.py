"""Compile production capture draw paths with a recording Vulkan renderer.

Run under Linux/WSL with g++. Generated C++ and binaries use TemporaryDirectory.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(path, signature):
    source = (ROOT / path).read_text(encoding='utf-8')
    start = source.index(signature)
    body = source.index('{', start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


render_methods = '\n'.join(
    function(f'src/renderer/{name}.cpp', f'{ret} {name}::{method}(')
    for name in ('CaptureRenderer', 'UsbCaptureRenderer')
    for ret, method in (('bool', 'render'), ('void', 'renderBlackTexture'))
)
layer_source = (ROOT / 'src/layer/LayerVideo_Render.cpp').read_text(encoding='utf-8')
start = layer_source.index('void LayerVideo::render()')
end = layer_source.index('  // 音频模式下', start)
main_capture = layer_source[start:end] + '\n}'
slice_capture = function('src/layer/LayerVideo_Render.cpp', 'bool LayerVideo::renderSliceCapture(')
kawase = (ROOT / 'src/renderer/VulkanRenderer_Kawase.cpp').read_text(encoding='utf-8')
selection = kawase[kawase.index('  const bool isYuvFormat ='):kawase.index('  // The conversion/blur pass')]

header = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#define LOG_WARN(...) ((void)0)
#define VK_NULL_HANDLE 0
using VkPipeline=int; using VkPipelineLayout=int; using VkDescriptorSet=int;
struct Texture {
 bool isDrmPrime=false,isV4L2Capture=false,isBGR3=false,isNV24=false,isYUYV=false,isNV12=false;
 int ycbcrConversion=0,ycbcrPipelineKey=0,descriptorSet=1;
 bool isAnyYuvTexture()const{return isDrmPrime||isV4L2Capture||isNV24||isYUYV||isNV12||ycbcrConversion;}
 bool usesNv12ShaderPath()const{return isNV12&&!isDrmPrime&&!isV4L2Capture;}
};
struct VulkanRenderer {
 float blur=-1;int fit=-1,draws=0;uint32_t texture=0;
 int getLogicalWidth(){return 1920;} int getLogicalHeight(){return 1080;}
 void setTextureContentCrop(uint32_t,bool,float,float,float,float,bool){}
 void renderLayer(uint32_t t,int,int,int,int,float,float,float,void*,int,float,bool,int,float b,int f){
   texture=t;blur=b;fit=f;++draws;
 }
};
struct CaptureRenderer {
 VulkanRenderer* renderer_=nullptr;
 bool textureUpdated_=true,blackTextureCreated_=true,cropEnabled_=false;
 uint32_t updatedTextureId_=7,blackTextureId_=9;
 float cropX_=0,cropY_=0,cropW_=1,cropH_=1;
 void setContentCrop(bool,float,float,float,float){}
 bool render(float,float,float,float,float,float,float,int,float,bool,int,float,int);
 void renderBlackTexture(float,float,float,float,float,float,float,int,float,bool,int,float,int);
};
struct UsbCaptureRenderer : CaptureRenderer {
 bool render(float,float,float,float,float,float,float,int,float,bool,int,float,int);
 void renderBlackTexture(float,float,float,float,float,float,float,int,float,bool,int,float,int);
};
int effectiveCaptureInvert(int invert,int){return invert;}
struct LayerVideo {
 struct SliceCaptureState {
  std::mutex mutex;std::atomic<bool> active{true},starting{false};std::string captureType;
  std::shared_ptr<CaptureRenderer> captureRenderer;
  std::shared_ptr<UsbCaptureRenderer> usbRenderer;
 };
 bool visible_=true,effectLinkedSlices_=false,blackToTransparent_=false;
 VulkanRenderer* renderer_=nullptr;CaptureRenderer* captureRenderer_=nullptr;
 UsbCaptureRenderer* usbRenderer_=nullptr;void* usbCapture_=nullptr;
 struct {int x=0,y=0;} position_;
 struct {int width=1920,height=1080;} size_;
 float rotation_=0,scale_=1,shapeParam_=0,gaussianBlur_=7;
 int shapeType_=0,invert_=0,fitMode_=1,layerId_=10;
 std::atomic<int> captureRotation_{0},audioEffectType_{0},audioEffectStackPacked_{0};
 std::mutex sliceCaptureMutex_;
 std::map<std::string,std::shared_ptr<SliceCaptureState>> sliceCaptures_;
 bool isCaptureLayer(){return true;}void ensureCapturePlaceholderRenderer(){}
 float getAlpha(){return 1;}float getCropLeft(){return 0;}float getCropRight(){return 0;}
 float getCropTop(){return 0;}float getCropBottom(){return 0;}
 int selectedShapeEffectId(int,int){return 0;}void applyAudioShapeEffect(int,int&,float&){}
 void render();
 bool renderSliceCapture(const std::string&,int,int,int,int,float,float,float,int,float,bool,int,float,int);
};
struct ConversionHarness {
 int drmPrimePipelineLayout_=11,v4l2PipelineLayout_=12,bgr3PipelineLayout_=13,
     nv24PipelineLayout_=14,yuyvPipelineLayout_=15,ycbcrPipelineLayout_=16,nv12PipelineLayout_=17;
 struct Resources {int pipelineLayout;};std::map<int,Resources> ycbcrPipelineCache_{{42,{18}}};
 std::string shader;int layout=0;
 int getKawaseConversionPipeline(int l,const char* s){layout=l;shader=s?s:"";return l;}
 int select(Texture inputTexture){
  const int textureId=-1;
'''
tests = r'''
  return yuvConvertPipeline;
 }
};
int main(){
 VulkanRenderer vk;CaptureRenderer hdmi;UsbCaptureRenderer usb;
 hdmi.renderer_=&vk;usb.renderer_=&vk;
 // Both actual renderer methods: real frames, no signal, zero/nonzero blur, fit modes.
 for(float blur:{0.0f,4.0f,10.0f})for(int fit:{0,1})for(bool signal:{false,true}){
  hdmi.textureUpdated_=usb.textureUpdated_=signal;
  bool h=hdmi.render(0,0,1920,1080,0,1,1,0,0,false,0,blur,fit);
  assert(h==signal && vk.blur==blur && vk.fit==fit && vk.texture==(signal?7u:9u));
  bool u=usb.render(0,0,1920,1080,0,1,1,0,0,false,0,blur,fit);
  assert(u==signal && vk.blur==blur && vk.fit==fit);
 }
 hdmi.textureUpdated_=usb.textureUpdated_=true;
 LayerVideo layer;layer.renderer_=&vk;layer.captureRenderer_=&hdmi;
 layer.render();assert(vk.blur==7 && vk.fit==1);
 layer.usbCapture_=&usb;layer.usbRenderer_=&usb;
 layer.render();assert(vk.blur==7 && vk.fit==1);
 auto state=std::make_shared<LayerVideo::SliceCaptureState>();
 layer.sliceCaptures_["slice"]=state;
 for(std::string type:{"HDMI","MIPI","USB","AUTO"}){
  state->captureType=type;
  state->captureRenderer=type=="AUTO"?nullptr:std::make_shared<CaptureRenderer>();
  state->usbRenderer=std::make_shared<UsbCaptureRenderer>();
  if(state->captureRenderer)state->captureRenderer->renderer_=&vk;
  state->usbRenderer->renderer_=&vk;
  assert(layer.renderSliceCapture("slice",0,0,800,600,0,1,1,0,0,false,0,3,0));
  assert(vk.blur==3 && vk.fit==0); // Slice overrides parent's blur 7/fit 1.
 }
 assert(!layer.renderSliceCapture("missing",0,0,800,600,0,1,1,0,0,false,0,3,0));
 ConversionHarness h;
 Texture t;t.isNV12=true;assert(h.select(t)==17 && h.shader=="nv12_texture.frag.spv");
 t={};t.isNV24=true;assert(h.select(t)==14 && h.shader=="nv24_texture.frag.spv");
 t={};t.isYUYV=true;assert(h.select(t)==15 && h.shader=="yuyv_texture.frag.spv");
 t={};t.isBGR3=true;assert(h.select(t)==13 && h.shader=="bgr3_texture.frag.spv");
 t={};t.isV4L2Capture=true;assert(h.select(t)==12 && h.shader=="texture.frag.spv");
 t.isDrmPrime=true;assert(h.select(t)==11); // Different immutable sampler layout.
 t={};t.ycbcrConversion=1;t.ycbcrPipelineKey=42;assert(h.select(t)==18);
 t.ycbcrPipelineKey=99;assert(h.select(t)==16);
}
'''
with tempfile.TemporaryDirectory(prefix='hsvj-capture-blur-') as temp:
    source = Path(temp) / 'capture.cpp'
    binary = Path(temp) / 'capture'
    source.write_text(header + selection + tests + render_methods + main_capture + slice_capture,
                      encoding='utf-8')
    subprocess.run(['g++', '-std=c++17', '-pthread', str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: capture frame/placeholder blur, main USB/HDMI, independent HDMI/MIPI/USB slices, format conversion selection')
