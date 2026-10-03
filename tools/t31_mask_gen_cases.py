#!/usr/bin/env python3
"""Random command sequences for tools/check_t31_mask_oracle.sh."""
import random, struct, sys
random.seed(int(sys.argv[1]))
out=[]
def mask():
    b=bytearray(0xac)
    for c in range(3):
        for k in range(4):
            o=c*0x38+k*14
            b[o]=random.choice([0,1,1,2,0xff])
            b[o+1]=random.randrange(256)
            struct.pack_into('<HHHH',b,o+2,*[random.choice([0,8,100,1000,1919,2559,65535,random.randrange(65536)]) for _ in range(4)])
            for i in range(10,14): b[o+i]=random.randrange(256)
    struct.pack_into('<I',b,0xa8,random.choice([0,1]))
    for i in range(0xa8,0xac): b[i]=random.randrange(256) if random.random()<.3 else b[i]
    return 'set '+' '.join('%02x'%x for x in b)
for c in range(3):
    w=[random.choice([0,1,2,5,0xffffffff]) if i in (0,3) else random.choice([0,640,1280,1920,2560,1440,random.randrange(1<<32)]) for i in range(13)]
    out.append('ds %d '%c+' '.join('%x'%x for x in w))
for r in (0x9968,0x9a68,0x9b68):
    out.append('rd %x %x'%(r,random.choice([0,1,0x80000000,3])))
out.append('chen %x'%random.choice([0,0xffffffff,0xf0007,0x38,0x12345678]))
if random.random()<.5: out.append('flip %x'%random.choice([0,1,2,3]))
out.append('get')
for _ in range(random.randrange(1,6)):
    r=random.random()
    if r<.4: out.append(mask())
    elif r<.9: out.append('flip %x'%random.choice([0,1,2,3,1,2,3,0xfd,0x41]))
    else: out.append('rd %x %x'%(random.choice((0x9968,0x9a68,0x9b68)),random.choice([0,1])))
    out.append('get')
print('\n'.join(out))
