/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ppc_mac_gpu_r300_metal.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static uint32_t reg(NSDictionary *registers, unsigned address)
{
    NSNumber *value = registers[[NSString stringWithFormat:@"%04x",address]];
    CHECK(value);
    return value.unsignedIntValue;
}
static void reset_target(uint8_t *target, unsigned width, unsigned height)
{
    for (unsigned i=0;i<width*height;i++) {
        target[i*4]=16; target[i*4+1]=32; target[i*4+2]=48; target[i*4+3]=64;
    }
}
static void check_pixels(const R300MetalDraw *d, const uint8_t *texture,
                         unsigned alpha, const char *name)
{
    const unsigned background[4]={16,32,48,64};
    unsigned count=0;
    for(unsigned y=0;y<d->height;y++) for(unsigned x=0;x<d->width;x++) {
        int tx=(int)x-(int)d->viewport_x,ty=(int)y-(int)d->viewport_y;
        bool inside=tx>=0 && ty>=0 && tx<256 && ty<256 && x>=d->scissor[0] && y>=d->scissor[1] &&
                    x-d->scissor[0]<d->scissor[2] && y-d->scissor[1]<d->scissor[3];
        for(unsigned c=0;c<4;c++) {
            unsigned want=background[c];
            if(inside && (d->color_mask & (1u<<c)))
                want=c==3 ? alpha : texture[(ty*256+tx)*4+c];
            if (inside && (d->color_mask & (1u<<c)) && d->premultiplied_over) {
                want += (background[c]*(255-alpha)+127)/255;
                if (want>255) want=255;
            }
            unsigned got=d->target[(y*d->width+x)*4+c];
            if(abs((int)got-(int)want)>1) {
                fprintf(stderr,"%s pixel (%u,%u) channel %u: got %u expected %u\n",name,x,y,c,got,want);
                exit(1);
            }
        }
        count++;
    }
    printf("PASS %s: %u pixels\n",name,count);
}
int main(int argc, char **argv)
{
    @autoreleasepool {
        CHECK(argc==2);
        NSString *root=[NSString stringWithUTF8String:argv[1]];
        NSData *json=[NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"draw1.json"]];
        CHECK(json);
        NSDictionary *fixture=[NSJSONSerialization JSONObjectWithData:json options:0 error:NULL];
        CHECK(fixture);
        NSDictionary *registers=fixture[@"registers"];
        NSDictionary *bank=fixture[@"vertex_bank"];
        NSArray *packet=fixture[@"packet"];
        CHECK(packet.count==49 && [packet[0] unsignedIntValue]==0x0004003d);
        CHECK(reg(registers,0x2150)==0x01030003 && reg(registers,0x2154)==0x00002203);
        CHECK(reg(registers,0x2090)==3 && reg(registers,0x2094)==4);
        CHECK(reg(registers,0x4300)==0x00040084 && reg(registers,0x4304)==0);
        CHECK(reg(registers,0x4310)==0x00d10000 && reg(registers,0x4330)==0x4048);
        CHECK(reg(registers,0x4480)==0x8007f8ff && reg(registers,0x44c0)==0xaa0c);
        CHECK(reg(registers,0x4500)==255 && reg(registers,0x4540)==0x600000);
        CHECK(reg(registers,0x4400)==0xa12 && !(reg(registers,0x4e04)&1));
        CHECK(reg(registers,0x1d98)==0x44000000 && reg(registers,0x1da0)==0xc3c00000);
        R300VPProgram vp={0}; R300FPProgram fp={0};
        float vc[256][4]={0}, fc[32][4]={0};
        for(unsigned i=0;i<3;i++) {
            fp.control[i]=reg(registers,0x4600+i*4);
            vp.control[i]=reg(registers,0x22d0+i*4);
        }
        for(unsigned i=0;i<4;i++) fp.node[i]=reg(registers,0x4610+i*4);
        for(unsigned i=0;i<32;i++) fp.tex[i]=reg(registers,0x4620+i*4);
        for(unsigned i=0;i<64;i++) fp.alu[i]=(R300FPInstruction){
            reg(registers,0x46c0+i*4),reg(registers,0x47c0+i*4),
            reg(registers,0x48c0+i*4),reg(registers,0x49c0+i*4)};
        for(NSString *key in bank) {
            unsigned index=(unsigned)strtoul(key.UTF8String,NULL,16);
            NSArray *words=bank[key]; CHECK(words.count==4);
            for(unsigned c=0;c<4;c++) {
                uint32_t word=[words[c] unsignedIntValue];
                if(index<256) {vp.code[index][c]=word;vp.code_valid[index]|=1u<<c;}
                if(index>=512 && index<768) memcpy(&vc[index-512][c],&word,4);
            }
        }
        for(unsigned i=0;i<32;i++) for(unsigned c=0;c<4;c++)
            CHECK(r300_float24_decode(reg(registers,0x4c00+i*16+c*4),&fc[i][c]));
        fp.alpha_func=reg(registers,0x4bd4);
        vp.varying_mask=fp.input_mask=3;
        vp.varying_output[0]=1;vp.varying_output[1]=2;
        uint32_t stream[8],ext[8],raw[48];
        for(unsigned i=0;i<8;i++) {stream[i]=reg(registers,0x2150+4*i);ext[i]=reg(registers,0x21e0+4*i);}
        for(unsigned i=0;i<48;i++) raw[i]=[packet[i+1] unsignedIntValue];
        R300VertexInput quads[4], triangles[6];
        CHECK(r300_decode_immediate(stream,ext,raw,48,4,quads,4,&vp.attribute_mask));
        const unsigned order[6]={0,1,2,0,2,3};
        for(unsigned i=0;i<6;i++) triangles[i]=quads[order[i]];
        NSData *tex=[NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"texture0.bin"]];
        CHECK(tex.length==256*256*4);
        uint8_t *rgba=malloc(tex.length);CHECK(rgba);
        const uint8_t *rawtex=tex.bytes;
        /* Captured format selects byte components 2,1,0 and literal one.
         * TEX_OFFSET swap and tile bits are all zero in this fixture. */
        for(unsigned i=0;i<256*256;i++) {
            rgba[i*4]=rawtex[i*4+2];rgba[i*4+1]=rawtex[i*4+1];
            rgba[i*4+2]=rawtex[i*4];rgba[i*4+3]=255;
        }
        const unsigned width=1024,height=768;
        uint8_t *pixels=malloc(width*height*4);CHECK(pixels);
        R300MetalDraw d={.vp=&vp,.fp=&fp,.vertices=triangles,.vertex_count=6,
            .vertex_constants=vc,.fragment_constants=fc,.target=pixels,
            .target_size=width*height*4,.width=width,.height=height,
            .scissor={0,0,width,height},.color_mask=15};
        d.textures[0]=(R300MetalTexture){.rgba=rgba,.size=tex.length,.width=256,.height=256};
        void *renderer=r300_metal_create();CHECK(renderer);char error[1024];
        reset_target(pixels,width,height);
        if(!r300_metal_draw(renderer,&d,error,sizeof(error))) {fprintf(stderr,"%s\n",error);exit(1);}
        check_pixels(&d,rgba,255,"captured Leopard quad, texture and shader replay");
        fc[0][3]=0.5f;
        reset_target(pixels,width,height);
        CHECK(r300_metal_draw(renderer,&d,error,sizeof(error)));
        check_pixels(&d,rgba,128,"updated constants with cached pipeline");
        for(unsigned i=0;i<256*256;i++) rgba[i*4]^=0xff;
        reset_target(pixels,width,height);
        CHECK(r300_metal_draw(renderer,&d,error,sizeof(error)));
        check_pixels(&d,rgba,128,"updated texture with cached pipeline");
        d.premultiplied_over=true;
        reset_target(pixels,width,height);
        CHECK(r300_metal_draw(renderer,&d,error,sizeof(error)));
        check_pixels(&d,rgba,128,"premultiplied-over RGB and alpha destination blend");
        d.premultiplied_over=false;
        reset_target(pixels,width,height);
        CHECK(r300_metal_draw(renderer,&d,error,sizeof(error)));
        check_pixels(&d,rgba,128,"blend-disabled pipeline restored without stale state");
        d.viewport_x=10;d.viewport_y=20;
        reset_target(pixels,width,height);
        CHECK(r300_metal_draw(renderer,&d,error,sizeof(error)));
        check_pixels(&d,rgba,128,"viewport origin translates pixels and preserves background");
        d.viewport_x=d.viewport_y=0;
        d.scissor[0]=32;d.scissor[1]=48;d.scissor[2]=64;d.scissor[3]=80;
        reset_target(pixels,width,height);
        CHECK(r300_metal_draw(renderer,&d,error,sizeof(error)));
        check_pixels(&d,rgba,128,"scissor preserves destination pixels");
        d.color_mask=2;
        reset_target(pixels,width,height);
        CHECK(r300_metal_draw(renderer,&d,error,sizeof(error)));
        check_pixels(&d,rgba,128,"color mask preserves destination channels");
        reset_target(pixels,width,height);
        fp.alu[2].rgb_inst |= 3u<<23;
        CHECK(!r300_metal_draw(renderer,&d,error,sizeof(error)));
        d.color_mask=0;
        check_pixels(&d,rgba,128,"unsupported shader leaves destination unchanged");
        memset(&fp,0,sizeof(fp));
        fp.control[1]=1;fp.control[2]=0x40;fp.node[3]=0x400000;
        fp.alu[0]=(R300FPInstruction){0x1c000020,0x01000020,0x00050a80,0x00040889};
        CHECK(r300_window_vertex_program(&vp,vc,512,512,-384,384));
        fc[0][0]=0.25f;fc[0][1]=0.5f;fc[0][2]=0.75f;fc[0][3]=1;
        d.color_mask=15;d.scissor[0]=d.scissor[1]=0;
        d.scissor[2]=width;d.scissor[3]=height;
        for(unsigned i=0;i<256*256;i++) {
            rgba[i*4]=64;rgba[i*4+1]=128;rgba[i*4+2]=191;rgba[i*4+3]=255;
        }
        reset_target(pixels,width,height);
        if(!r300_metal_draw(renderer,&d,error,sizeof(error))) {fprintf(stderr,"%s\n",error);exit(1);}
        check_pixels(&d,rgba,255,"captured TCL-bypass solid shader with window-coordinate adapter");

        R300VertexInput point={0};point.a[0][0]=512;point.a[0][1]=384;point.a[0][3]=1;
        CHECK(r300_window_vertex_program(&vp,vc,512,512,-384,384));
        vp.point_size=64;
        d.vertices=&point;d.vertex_count=1;d.primitive_points=true;
        reset_target(pixels,width,height);
        CHECK(r300_metal_draw(renderer,&d,error,sizeof(error)));
        unsigned point_pixels=0;
        for(unsigned y=0;y<height;y++) for(unsigned x=0;x<width;x++) {
            bool inside=x>=480 && x<544 && y>=352 && y<416;
            const unsigned fill[4]={64,128,191,255},background[4]={16,32,48,64};
            for(unsigned c=0;c<4;c++) {
                unsigned got=pixels[(y*width+x)*4+c];
                unsigned want=inside ? fill[c] : background[c];
                CHECK(abs((int)got-(int)want)<=1);
            }
            point_pixels+=inside;
        }
        CHECK(point_pixels==4096);
        puts("PASS programmable 64x64 Metal point coverage and destination preservation");
        r300_metal_destroy(renderer);free(rgba);free(pixels);
    }
    return 0;
}
