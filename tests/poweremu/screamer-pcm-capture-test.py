#!/usr/bin/env python3
"""Validate bounded raw DMA capture, wrap order and stopped-only flushing."""
from pathlib import Path
import ast, subprocess, tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'hw/audio/screamer.c').read_text()
helpers=s[s.index('#define SCREAMER_PCM_BYTES'):s.index('/* Opt-in, bounded producer trace.')]
helpers=helpers.replace('(128 * 1024 * 1024)','64').replace('#define SCREAMER_PCM_RECORDS 1000000','#define SCREAMER_PCM_RECORDS 3')
tree=ast.parse((root/'tests/poweremu/screamer-trace-test.py').read_text())
stub=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='stub' for t in n.targets))
stub=stub.replace('uint32_t regs[6], wpos, rpos, rate;','uint32_t regs[6], wpos, rpos, rate; int shift,samples; uint8_t *mixbuf;')
stub+='\n#define MIN(a,b) ((a)<(b)?(a):(b))\n'
main=r"""
int main(int argc,char **argv) {
 _Static_assert(sizeof(ScreamerPCMRecord)==48,"metadata ABI");
 uint8_t ring[16];for(int i=0;i<16;i++)ring[i]=i;
 ScreamerState s={.io={0x1234,16},.regs={0,0,0,0,0,777},.wpos=3,.rate=44100,.shift=2,.samples=4,.mixbuf=ring};
 unsetenv("POWEREMU_SCREAMER_PCM_CAPTURE");screamer_pcm_init();screamer_pcm_capture(&s,2);
 assert(!screamer_pcm.bytes && clock_reads==0);
 setenv("POWEREMU_SCREAMER_PCM_CAPTURE","relative",1);screamer_pcm_init();assert(!screamer_pcm.bytes);
 setenv("POWEREMU_SCREAMER_PCM_CAPTURE",argv[1],1);screamer_pcm_init();assert(screamer_pcm.bytes && exit_notify);
 char *path=strdup(screamer_pcm.path);screamer_pcm_capture(&s,2);
 assert(screamer_pcm.used==8 && screamer_pcm.count==1);
 assert(!memcmp(screamer_pcm.bytes,ring+12,4) && !memcmp(screamer_pcm.bytes+4,ring,4));
 assert(screamer_pcm.records[0].frame_count==777 && screamer_pcm.records[0].offset==0);
 assert(screamer_pcm.records[0].dma_addr==0x1234 && screamer_pcm.records[0].rate==44100);
 assert(screamer_pcm.records[0].shift==2 && screamer_pcm.records[0].frames==2);
 screamer_pcm_capture(&s,4);screamer_pcm_capture(&s,4);screamer_pcm_capture(&s,4);
 assert(screamer_pcm.count==3 && screamer_pcm.dropped_records==1 && screamer_pcm.dropped_bytes==16);
 /* Exercise byte capacity independently of record capacity. */
 screamer_pcm.used=63;screamer_pcm.count=2;screamer_pcm_capture(&s,2);
 assert(screamer_pcm.used==63 && screamer_pcm.count==2 && screamer_pcm.dropped_bytes==24);
 screamer_pcm.used=40;screamer_pcm.count=3;
 assert(access(path,F_OK)!=0);exit_notify->notify(exit_notify,NULL);
 assert(!screamer_pcm.bytes && !screamer_pcm.records && !screamer_pcm.path);
 FILE *f=fopen(path,"rb");assert(f);uint8_t result[40];assert(fread(result,1,40,f)==40 && fgetc(f)==EOF);fclose(f);
 assert(!memcmp(result,ring+12,4) && !memcmp(result+4,ring,4));
 char *events=g_strdup_printf("%s.events",path);f=fopen(events,"rb");assert(f);ScreamerPCMRecord records[3];assert(fread(records,sizeof(records),1,f)==1 && fgetc(f)==EOF);fclose(f);
 assert(records[1].offset==8 && records[2].offset==24 && records[0].wall_ns==88);
 char *meta=g_strdup_printf("%s.json",path);f=fopen(meta,"r");assert(f);char json[512]={0};assert(fread(json,1,511,f)>0);fclose(f);
 assert(strstr(json,"\"dropped_bytes\":24") && strstr(json,"\"pcm_flush_ok\":true") && strstr(json,"\"events_flush_ok\":true"));
 unlink(path);unlink(events);unlink(meta);free(path);free(events);free(meta);
 puts("bounded raw DMA capture: PASS");return 0;
}
"""
with tempfile.TemporaryDirectory() as d:
 d=Path(d);src=d/'capture.c';src.write_text(stub+helpers+main);binary=d/'capture'
 subprocess.run(['clang','-D_GNU_SOURCE','-g','-fsanitize=address,undefined',str(src),'-o',str(binary)],check=True)
 subprocess.run([str(binary),str(d/'capture')],check=True)
