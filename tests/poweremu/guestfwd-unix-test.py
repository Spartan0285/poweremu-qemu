#!/usr/bin/env python3
"""Exercise actual guestfwd parser: independent Unix sockets and old backends."""
from pathlib import Path
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'net/slirp.c').read_text()
def function(signature):
    begin = source.index(signature)
    end = source.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]
code = r'''
#include <glib.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
typedef struct Error {int unused;} Error;
typedef struct Chardev {int unused;} Chardev;
typedef struct {int unused;} CharBackend;
typedef struct {void *slirp; GSList *fwd;} SlirpState;
struct GuestFwd {CharBackend hd; struct in_addr server; int port; void *slirp;};
static unsigned unix_calls, exec_calls, char_calls;
static int result;
static char path[256];
static void error_setg(Error **err,const char *fmt,...) {*err=g_new0(Error,1);}
static void error_propagate(Error **err,Error *e) {*err=e;}
static int slirp_add_unix(void *s,const char *p,struct in_addr *a,int port) {
    unix_calls++;assert(port==7701);assert(a->s_addr==inet_addr("10.0.2.100"));
    g_strlcpy(path,p,sizeof(path));return result;
}
static int slirp_add_exec(void *s,const char *p,struct in_addr *a,int port) {
    exec_calls++;g_strlcpy(path,p,sizeof(path));return result;
}
static Chardev *qemu_chr_new_mux_mon(const char *id,const char *p,void *unused) {
    static Chardev chr;char_calls++;g_strlcpy(path,p,sizeof(path));return &chr;
}
static void qemu_chr_fe_init(CharBackend *b,Chardev *c,Error **e) {}
static void qemu_chr_fe_deinit(CharBackend *b,bool close) {}
#define OBJECT(c) (c)
static void object_unparent(void *o) {}
static int slirp_add_guestfwd(void *s,void *cb,void *b,struct in_addr *a,int p){return 0;}
#define guestfwd_write NULL
#define guestfwd_can_read NULL
#define guestfwd_read NULL
static void qemu_chr_fe_set_handlers(CharBackend *b,void *a,void *c,void *d,void *e,void *f,void *g,bool h) {}
''' + function('static int get_str_sep(') + '\n' + function('static int slirp_guestfwd(SlirpState *s, const char *config_str, Error **errp)\n{') + r'''
static void check(const char *rule,int expected) {
    SlirpState s={0};Error *err=NULL;
    assert(slirp_guestfwd(&s,rule,&err)==expected);
    assert((err!=NULL)==(expected<0));g_free(err);
    g_slist_free_full(s.fwd,g_free);
}
int main(void) {
    check("tcp:10.0.2.100:7701-unix-connect:/tmp/a b.sock",0);
    assert(unix_calls==1&&!strcmp(path,"/tmp/a b.sock"));
    check("tcp:10.0.2.100:7701-unix-connect:",-1);assert(unix_calls==1);
    check("tcp:10.0.2.100:65536-unix-connect:/tmp/x",-1);assert(unix_calls==1);
    check("tcp:garbage:7701-unix-connect:/tmp/x",-1);assert(unix_calls==1);
    result=-1;check("tcp:10.0.2.100:7701-unix-connect:/tmp/x",-1);assert(unix_calls==2);
    result=0;check("tcp:10.0.2.100:7701-unix:/tmp/old",0);
    assert(char_calls==1&&unix_calls==2&&!strcmp(path,"unix:/tmp/old"));
    check("tcp:10.0.2.100:7701-cmd:/usr/bin/nc -U /tmp/old",0);
    assert(exec_calls==1&&!strcmp(path,"/usr/bin/nc -U /tmp/old"));
    puts("PASS independent Unix forwarding, path parsing, rejection, legacy char/cmd behavior");
}
'''
flags = shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True))
with tempfile.TemporaryDirectory(prefix='poweremu-guestfwd-') as tmp:
    src=Path(tmp)/'test.c';src.write_text(code);exe=Path(tmp)/'test'
    subprocess.run(['clang','-g','-O1','-fsanitize=address,undefined',str(src),'-o',str(exe),*flags],check=True)
    subprocess.run([str(exe)],check=True)
