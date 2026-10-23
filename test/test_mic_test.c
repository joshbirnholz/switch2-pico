#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mic_test.h"

static mic_test_status_t status(uint32_t id,uint32_t now) {
    mic_test_status_t s;mic_test_status(id,now,&s);return s;
}
static uint32_t recording(void) {
    uint32_t id=mic_test_open(0);assert(id);
    assert(mic_test_record(id,0));
    mic_test_tick(600,3,false);
    assert(status(id,600).phase==MIC_TEST_RECORDING);
    return id;
}
int main(void) {
    uint8_t frame[S2_MIC_FRAME_BYTES]={0}, out[S2_MIC_FRAME_BYTES];
    uint32_t owner=0;
    assert(mic_test_supported());
    uint32_t id=mic_test_open(100);
    assert(id && mic_test_active() && mic_test_input());
    assert(!mic_test_open(110)); // another UI cannot steal an active test
    assert(!mic_test_record(id+1,120));
    assert(!mic_test_cancel(id+1));
    mic_test_level(8000,0,120);
    assert(status(id,130).peak==8000);
    assert(status(id,500).peak==0); // stale level doesn't stick
    assert(mic_test_cancel(id) && !mic_test_active());
    id=recording();
    for(uint32_t i=0;i<250;i++) {
        uint32_t now=600+i*20;frame[0]=(uint8_t)i;
        mic_test_frame(frame,sizeof frame,now);
        mic_test_tick(now,3,false);
        if(i%100==0) assert(status(id,now).phase==MIC_TEST_RECORDING);
    }
    // Exact five-second boundary must not capture a 251st frame.
    mic_test_frame(frame,sizeof frame,5600);
    mic_test_tick(5600,3,false);
    assert(status(id,5600).phase==MIC_TEST_PLAYBACK && !mic_test_input());
    for(uint32_t i=0;i<250;i++) {
        assert(mic_test_play_frame(out,&owner,5600+i*20));
        assert(owner==id && out[0]==(uint8_t)i);
        if(i%100==0) (void)status(id,5600+i*20);
    }
    assert(!mic_test_play_frame(out,&owner,10600));
    mic_test_tick(10880,3,false);assert(status(id,10880).phase==MIC_TEST_DONE);
    assert(!mic_test_active());
    id=mic_test_open(0);mic_test_tick(5001,3,false);
    assert(status(0,5001).phase==MIC_TEST_ERROR && !mic_test_active()); // lost UI
    id=mic_test_open(0);mic_test_tick(1,0,false);
    assert(status(id,1).phase==MIC_TEST_ERROR); // unplug
    id=mic_test_open(0);mic_test_tick(1,3,true);
    assert(status(id,1).phase==MIC_TEST_ERROR); // another audio app
    id=mic_test_open(0);(void)status(id,2500);mic_test_tick(3001,3,false);
    assert(status(id,3001).phase==MIC_TEST_ERROR); // no frames
    id=recording();(void)status(id,4000);mic_test_frame(frame,sizeof frame,5500);
    mic_test_tick(5600,3,false);
    assert(status(id,5600).phase==MIC_TEST_ERROR); // incomplete capture
    id=mic_test_open(0);assert(mic_test_record(id,0));
    mic_test_frame(frame,sizeof frame,600);mic_test_tick(600,1,false);
    assert(status(id,600).phase==MIC_TEST_PREPARING); // headphones-only during warm-up
    mic_test_frame(frame,sizeof frame,1100);mic_test_tick(1100,3,false);
    assert(status(id,1100).phase==MIC_TEST_RECORDING); // valid mic arrives later
    mic_test_cancel(id);
    id=mic_test_open(0);assert(mic_test_record(id,0));
    mic_test_frame(frame,sizeof frame,3000);mic_test_tick(3000,1,false);
    assert(status(id,3000).phase==MIC_TEST_ERROR); // actual headphones-only
    uint32_t old=id;id=mic_test_open(10000);
    mic_test_fail(old,"stale decoder error");
    assert(status(id,10000).phase==MIC_TEST_READY); // stale session can't affect new test
    mic_test_fail(id,"decoder error");
    assert(status(id,10000).phase==MIC_TEST_ERROR);
    puts("Microphone test sessions/capture/playback/cleanup: passed");
}
