#!/usr/bin/env python3
"""Exercise the production cache past capacity without requiring a GPU."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/display/ppc_mac_gpu_r300_metal.m').read_text()
cache = source[source.index('@interface R300LRUEntry'):source.index('typedef struct R300MetalContext')]
main = r'''
int main(void) {
    @autoreleasepool {
        R300LRUCache *cache = [[R300LRUCache alloc] initWithCapacity:64];
        for (unsigned i = 0; i < 63; ++i) {
            NSString *key = [NSString stringWithFormat:@"hot%u", i];
            [cache setObject:key forKey:key];
        }
        for (unsigned cycle = 0; cycle < 1000; ++cycle) {
            @autoreleasepool {
                for (unsigned i = 0; i < 63; ++i) {
                    NSString *key = [NSString stringWithFormat:@"hot%u", i];
                    assert([[cache objectForKey:key] isEqual:key]);
                }
                NSString *cold = [NSString stringWithFormat:@"cold%u", cycle];
                [cache setObject:cold forKey:cold];
                assert(cache.count == 64);
                if (cycle) {
                    NSString *previous = [NSString stringWithFormat:@"cold%u", cycle-1];
                    assert([cache objectForKey:previous] == nil);
                }
            }
        }
        [cache setObject:@"replacement" forKey:@"hot62"];
        assert(cache.count == 64);
        assert([[cache objectForKey:@"hot62"] isEqual:@"replacement"]);
        NSMutableString *mutableKey = [NSMutableString stringWithString:@"immutable"];
        [cache setObject:@"copied" forKey:mutableKey];
        [mutableKey appendString:@"-changed"];
        assert([[cache objectForKey:@"immutable"] isEqual:@"copied"]);
        assert([cache objectForKey:mutableKey] == nil);
        NSMutableString *query = [NSMutableString stringWithString:@"immutable"];
        assert([[cache objectForKey:query] isEqual:@"copied"]);
        [query appendString:@"-changed"];
        for (unsigned i = 0; i < 64; ++i) {
            NSString *key = [NSString stringWithFormat:@"evict%u", i];
            [cache setObject:key forKey:key];
        }
        assert(cache.count == 64 && [cache objectForKey:@"immutable"] == nil);
        [cache release];
        R300LRUCache *single = [[R300LRUCache alloc] initWithCapacity:0];
        [single setObject:@1 forKey:@"a"];
        [single setObject:@2 forKey:@"b"];
        assert(single.count == 1 && [single objectForKey:@"a"] == nil);
        assert([[single objectForKey:@"b"] isEqual:@2]);
        [single release];
    }
    puts("PASS: bounded LRU retains 63 hot programs across 1000 cold insertions, replacement and immutable keys");
}
'''
with tempfile.TemporaryDirectory(prefix='poweremu-cache-') as tmp:
    p = Path(tmp)
    (p/'test.m').write_text('#import <Foundation/Foundation.h>\n#include <assert.h>\n'+cache+main)
    subprocess.run(['clang','-fno-objc-arc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.m'),'-framework','Foundation','-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
