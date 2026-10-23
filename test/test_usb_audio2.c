#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "usb_audio2.h"
static unsigned rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const uint8_t *p) { return rd16(p) | rd16(p+2) << 16; }
int main(void) {
    uint8_t b[256], r[32];
    memset(b, 0xa5, sizeof b);
    assert(!audio2_descriptor(b, AUDIO2_DESCRIPTOR_LENGTH-1, 2, 5, 0x86, 0x87, 5));
    assert(b[0] == 0xa5);
    unsigned n = audio2_descriptor(b, sizeof b, 2, 5, 0x86, 0x87, 5);
    assert(n == AUDIO2_DESCRIPTOR_LENGTH && b[n] == 0xa5);
    unsigned pos=0, cls_end=0, clock_ids=0, terminal_ids=0, interfaces=0, eps=0;
    unsigned speaker_clock=0, mic_clock=0, ac_eps=0;
    for (;pos<n;pos+=b[pos]) {
        const uint8_t *d=b+pos;
        assert(d[0] >= 2 && pos+d[0]<=n);
        if (d[1]==11) assert(d[3]==AUDIO2_INTERFACE_COUNT && d[6]==0x20);
        if (d[1]==4) {
            assert(d[7]==0x20);
            if (d[3]==0) interfaces++;
            if (d[2]==2) ac_eps=d[4];
        }
        if (d[1]==0x24 && d[2]==1 && d[0]==9) {
            assert(rd16(d+3)==0x200); cls_end=pos+rd16(d+6);
        }
        if (d[1]==0x24 && d[2]==10) {
            assert(d[5]==5); // frequency/validity read-only
            clock_ids|=1u<<d[3];
        }
        if (d[1]==0x24 && (d[2]==2 || d[2]==3) && (d[0]==17 || d[0]==12)) {
            assert(!(terminal_ids&(1u<<d[3])));terminal_ids|=1u<<d[3];
            if(d[3]==1) speaker_clock=d[7];
            if(d[3]==3) {mic_clock=d[7]; assert(rd16(d+14)==4);}
            if(d[3]==2) assert(rd16(d+9)==4);
        }
        if (d[1]==5) {
            eps++;
            if(d[2]==0x87) assert(pos==cls_end && d[3]==3 && rd16(d+4)==6);
            if(d[2]==5) assert(d[3]==9 && rd16(d+4)==196);
            if(d[2]==0x86) assert(d[3]==5 && rd16(d+4)==98);
        }
    }
    assert(pos==n && ac_eps==1 && interfaces==AUDIO2_INTERFACE_COUNT);
    assert(clock_ids&(1u<<speaker_clock));
#if S2_HEADSET_MIC
    assert(eps==3 && mic_clock!=speaker_clock && (clock_ids&(1u<<mic_clock)));
#else
    (void)mic_clock; assert(eps==2);
#endif
    assert(audio2_get(5,1,0,1,0,0,r,sizeof r)==4 && rd32(r)==48000);
    assert(audio2_get(5,1,0,2,0,0,r,sizeof r)==14);
    assert(rd16(r)==1 && rd32(r+2)==48000 && rd32(r+6)==48000 && rd32(r+10)==0);
    assert(audio2_get(5,2,0,1,0,0,r,sizeof r)==1 && r[0]==1);
    assert(!audio2_get(5,1,1,1,0,0,r,sizeof r));
    assert(!audio2_get(5,1,0,3,0,0,r,sizeof r));
    assert(!audio2_get(5,1,0,1,0,0,r,3));
    assert(!audio2_get(1,2,0,1,1,1,r,sizeof r));
    assert(!audio2_get(99,2,0,1,1,1,r,sizeof r));
    for(unsigned state=0;state<4;state++) {
        assert(audio2_get(2,2,0,1,state&1,state&2,r,sizeof r)==6);
        assert(r[0]==((state&1)?2:0) && rd32(r+1)==((state&1)?3:0) && r[5]==0);
#if S2_HEADSET_MIC
        assert(audio2_get(3,2,0,1,state&1,state&2,r,sizeof r)==6);
        assert(r[0]==((state&2)?1:0) && rd32(r+1)==0 && r[5]==0);
        assert(audio2_get(6,1,0,1,0,0,r,sizeof r)==4 && rd32(r)==48000);
#else
        assert(!audio2_get(3,2,0,1,1,1,r,sizeof r));
        assert(!audio2_get(6,1,0,1,0,0,r,sizeof r));
#endif
    }
    audio2_notification(r,2,3);
    const uint8_t notification[]={0,1,0,2,2,3};
    assert(!memcmp(r,notification,6));
    puts("USB Audio 2 descriptors/controls: passed");
}
