"""Compile the production decoder against an MPP shim with oversized reused buffers."""
from pathlib import Path
import subprocess
import tempfile
import sys

root = Path(__file__).resolve().parents[1]
shim = r'''
#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
using RK_S64 = int64_t;
using RK_U32 = uint32_t;
using MPP_RET = int;
using MppFrameFormat = int;
constexpr int MPP_OK=0, MPP_CTX_DEC=0, MPP_VIDEO_CodingMJPEG=1;
constexpr int MPP_BUFFER_TYPE_DRM=1, MPP_BUFFER_FLAGS_DMA32=2, MPP_BUFFER_FLAGS_CACHABLE=4;
constexpr int MPP_SET_INPUT_TIMEOUT=1, MPP_SET_OUTPUT_TIMEOUT=2, MPP_DEC_SET_PARSER_SPLIT_MODE=3;
constexpr int MPP_DEC_SET_IMMEDIATE_OUT=4, MPP_DEC_SET_OUTPUT_FORMAT=5, MPP_DEC_SET_INFO_CHANGE_READY=6;
constexpr int MPP_FMT_YUV420SP=0, MPP_FMT_YUV422SP=1, MPP_FRAME_FMT_MASK=0xff;
constexpr int KEY_OUTPUT_FRAME=0, KEY_INPUT_PACKET=1;
struct Buffer { std::vector<uint8_t> bytes; int refs=1; };
using MppBuffer=Buffer*;
using MppBufferGroup=void*;
struct Packet;
struct Frame;
struct Meta { Frame* frame=nullptr; Packet* packet=nullptr; };
struct Packet { MppBuffer buffer; size_t size,length; Meta meta; };
struct Frame { MppBuffer buffer=nullptr; Meta meta; };
using MppFrame=Frame*;
using MppPacket=Packet*;
using MppMeta=Meta*;
using MppCtx=void*;
struct MppApi {
  MPP_RET (*reset)(MppCtx);
  MPP_RET (*control)(MppCtx,int,void*);
  MPP_RET (*decode_put_packet)(MppCtx,MppPacket);
  MPP_RET (*decode_get_frame)(MppCtx,MppFrame*);
};
inline size_t expectedSize=0;
inline int submissions=0, allocations=0, liveBuffers=0;
inline Frame* ready=nullptr;
inline MPP_RET mpp_buffer_group_get_internal(MppBufferGroup* group,int) { *group=(void*)1; return 0; }
inline MPP_RET mpp_buffer_group_limit_config(MppBufferGroup,size_t,int) { return 0; }
inline MPP_RET mpp_buffer_group_put(MppBufferGroup) { return 0; }
inline MPP_RET mpp_buffer_get(MppBufferGroup,MppBuffer* buffer,size_t size) {
  // Model input/output pool reuse: capacity has no relationship to JPEG length.
  *buffer=new Buffer{std::vector<uint8_t>(std::max<size_t>(4177920,size),0xa5)};
  ++allocations; ++liveBuffers; return 0;
}
inline MPP_RET mpp_buffer_put(MppBuffer buffer) {
  if (--buffer->refs==0) { delete buffer; --liveBuffers; } return 0;
}
inline MPP_RET mpp_buffer_write(MppBuffer buffer,size_t offset,void* data,size_t size) {
  assert(offset+size<=buffer->bytes.size()); memcpy(buffer->bytes.data()+offset,data,size); return 0;
}
inline int mpp_buffer_get_fd(MppBuffer) { return 123; }
inline MPP_RET mpp_packet_init_with_buffer(MppPacket* packet,MppBuffer buffer) {
  ++buffer->refs;
  *packet=new Packet{buffer,buffer->bytes.size(),buffer->bytes.size(),{}}; return 0;
}
inline MPP_RET mpp_packet_deinit(MppPacket* packet) {
  mpp_buffer_put((*packet)->buffer); delete *packet; *packet=nullptr; return 0;
}
inline void mpp_packet_set_size(MppPacket packet,size_t size) { packet->size=size; }
inline void mpp_packet_set_length(MppPacket packet,size_t length) { packet->length=length; }
inline void mpp_packet_set_pts(MppPacket,int) {}
inline MppMeta mpp_packet_get_meta(MppPacket packet) { return &packet->meta; }
inline MPP_RET mpp_frame_init(MppFrame* frame) { *frame=new Frame; return 0; }
inline void mpp_frame_set_buffer(MppFrame frame,MppBuffer buffer) { ++buffer->refs; frame->buffer=buffer; }
inline MPP_RET mpp_frame_deinit(MppFrame* frame) {
  if ((*frame)->buffer) mpp_buffer_put((*frame)->buffer);
  delete *frame; *frame=nullptr; return 0;
}
inline MppMeta mpp_frame_get_meta(MppFrame frame) { return &frame->meta; }
inline MppBuffer mpp_frame_get_buffer(MppFrame frame) { return frame->buffer; }
inline unsigned mpp_frame_get_width(MppFrame) { return 1920; }
inline unsigned mpp_frame_get_height(MppFrame) { return 1080; }
inline unsigned mpp_frame_get_hor_stride(MppFrame) { return 1920; }
inline unsigned mpp_frame_get_ver_stride(MppFrame) { return 1088; }
inline unsigned mpp_frame_get_fmt(MppFrame) { return MPP_FMT_YUV420SP; }
inline unsigned mpp_frame_get_info_change(MppFrame) { return 0; }
inline unsigned mpp_frame_get_errinfo(MppFrame) { return 0; }
inline unsigned mpp_frame_get_discard(MppFrame) { return 0; }
inline MPP_RET mpp_meta_set_frame(MppMeta meta,int,MppFrame frame) { meta->frame=frame; return 0; }
inline MPP_RET mpp_meta_get_packet(MppMeta meta,int,MppPacket* packet) {
  *packet=meta->packet; meta->packet=nullptr; return 0;
}
inline MPP_RET put(MppCtx,MppPacket packet) {
  assert(packet->size==expectedSize && packet->length==expectedSize);
  assert(packet->buffer->bytes.size()>packet->size);
  assert(packet->buffer->bytes[packet->size-2]==0xff && packet->buffer->bytes[packet->size-1]==0xd9);
  ready=packet->meta.frame; ready->meta.packet=packet; ++submissions; return 0;
}
inline MPP_RET get(MppCtx,MppFrame* frame) { *frame=ready; ready=nullptr; return 0; }
inline MPP_RET reset(MppCtx) { return 0; }
inline MPP_RET control(MppCtx,int,void*) { return 0; }
inline MPP_RET mpp_create(MppCtx* ctx,MppApi** api) {
  static MppApi instance{reset,control,put,get}; *ctx=(void*)1; *api=&instance; return 0;
}
inline MPP_RET mpp_init(MppCtx,int,int) { return 0; }
inline MPP_RET mpp_destroy(MppCtx) { return 0; }
'''
test = r'''
#include "utils/RkmppMjpegDecoder.h"
#include "rk_mpi.h"
#include <iostream>
#include <fstream>
#include <iterator>
using namespace hsvj;
int main(int argc, char **argv) {
  RkmppMjpegDecoder decoder;
  assert(decoder.initialize());
  RkmppDecodedFrame output;
  for (size_t size : {size_t(700),size_t(4096),size_t(180000),size_t(65536)}) {
    std::vector<uint8_t> jpeg(size,0x21);
    jpeg[0]=0xff; jpeg[1]=0xd8; jpeg[size-2]=0xff; jpeg[size-1]=0xd9;
    expectedSize=size;
    assert(decoder.decode(jpeg.data(),jpeg.size(),output));
    assert(output.width==1920 && output.dmaBufFd==123);
    RkmppMjpegDecoder::releaseFrame(output);
    // UVC padding is excluded from the decoder packet.
    jpeg.insert(jpeg.end(),63,0x42);
    assert(decoder.decode(jpeg.data(),jpeg.size(),output));
    RkmppMjpegDecoder::releaseFrame(output);
    // MacroSilicon sample has D9 FF in place of FF D9; never alter caller bytes.
    jpeg.resize(size);
    jpeg[size-2]=0xd9; jpeg[size-1]=0xff;
    expectedSize=size;
    assert(decoder.decode(jpeg.data(),jpeg.size(),output));
    assert(jpeg[size-2]==0xd9 && jpeg[size-1]==0xff);
    RkmppMjpegDecoder::releaseFrame(output);
  }
  int before=allocations;
  for (const auto& bad : std::vector<std::vector<uint8_t>>{
        {},{0xff},
        {0x01,0x02,0xff,0xd9},{0xff,0xd8,0,0,0},{0xff,0xd8,0xff}}) {
    assert(!decoder.decode(bad.data(),bad.size(),output));
    assert(output.mppFrame==nullptr && output.dmaBufFd==-1);
  }
  assert(allocations==before && submissions==12 && liveBuffers==0);
  for (int index=1; index<argc; ++index) {
    std::ifstream file(argv[index],std::ios::binary);
    assert(file.is_open());
    std::vector<uint8_t> jpeg((std::istreambuf_iterator<char>(file)),{});
    assert(jpeg.size()>4);
    const auto original=jpeg;
    expectedSize=jpeg.size();
    const bool reversed=jpeg[jpeg.size()-2]==0xd9 && jpeg.back()==0xff;
    assert(decoder.decode(jpeg.data(),jpeg.size(),output)==reversed);
    assert(jpeg==original);
    if (reversed) RkmppMjpegDecoder::releaseFrame(output);
    assert(liveBuffers==0);
    std::cout << "PASS captured UVC frame: " << argv[index]
              << (reversed ? " reversed EOI repaired" : " truncation rejected") << "\n";
  }
  decoder.shutdown();
  std::cout << "PASS production MJPEG decoder: oversized pool reuse, exact packet size/length, reversed-EOI repair, UVC padding, truncated/invalid rejection, caller bytes unchanged, resource release\n";
}
'''
with tempfile.TemporaryDirectory(prefix='hsvj-usb-mjpeg-') as folder:
    temp = Path(folder)
    (temp/'utils').mkdir()
    (temp/'rk_mpi.h').write_text(shim)
    for name in ['mpp_buffer.h','mpp_frame.h','mpp_packet.h','mpp_meta.h']:
        (temp/name).write_text('#include "rk_mpi.h"\n')
    (temp/'utils/Logger.h').write_text('#define LOG_INFO(...) ((void)0)\n#define LOG_WARN(...) ((void)0)\n#define LOG_ERROR(...) ((void)0)\n')
    (temp/'test.cpp').write_text(test)
    executable = temp/'test'
    subprocess.run(['g++','-std=c++17','-D__ANDROID__','-I'+str(temp),'-I'+str(root/'include'),
                    str(root/'src/utils/RkmppMjpegDecoder.cpp'),str(temp/'test.cpp'),'-o',str(executable)],check=True)
    subprocess.run([str(executable),*sys.argv[1:]],check=True)
