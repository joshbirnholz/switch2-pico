// Inspect actual per-link state and bounded idle probe without a radio.
#include <assert.h>
#include <stdio.h>
#include "../core/src/s2_link.c"
static uint32_t test_now;
static unsigned switches;
static bool switch_on;
uint32_t platform_millis(void) { return test_now; }
void log_printf(const char *fmt, ...) { (void)fmt; }
bool s2t_set_audio_in(uint8_t link, bool on) {
    (void)link; switches++; switch_on=on; return true;
}
int main(void) {
    link_t *k=&s_links[0];
    k->pid=S2_PID_PRO2;k->state=S2_LINK_READY;
    assert(s2_link_headset_jack()==0);
    k->headset=true;
    assert(s2_link_headset_jack()==1);
    k->jack_probe_after=500;
    test_now=100;observe_headset_jack(k,5);
    assert(!k->jack_known && s2_link_headset_jack()==1 && s2_link_headset_detecting());
    test_now=200;observe_headset_jack(k,7);
    test_now=501;observe_headset_jack(k,7);
    assert(k->jack_known && s2_link_headset_jack()==3 && !s2_link_headset_detecting());
    test_now=600;observe_headset_jack(k,5);
    assert(s2_link_headset_jack()==3); // initial headphones-only report is provisional
    test_now=650;observe_headset_jack(k,7);
    assert(s2_link_headset_jack()==3);
    test_now=700;observe_headset_jack(k,5);
    test_now=1001;observe_headset_jack(k,5);
    assert(s2_link_headset_jack()==1);
    test_now=1100;observe_headset_jack(k,15);
    test_now=1401;observe_headset_jack(k,15);
    assert(s2_link_headset_jack()==3);
    k->headset=false;
    observe_headset_jack(k,7);
    assert(!k->headset_mic && s2_link_headset_jack()==0);
    k->headset=k->headset_mic=true;k->state=S2_LINK_SCANNING;
    assert(s2_link_headset_jack()==0);
    k->state=S2_LINK_READY;k->pid=S2_PID_JOYCON2_L;
    assert(s2_link_headset_jack()==0);
    k->pid=S2_PID_PRO2; k->headset=true; k->jack_known=false;
    k->jack_probe_until=1500; k->jack_probe_after=500; k->jack_candidate=0; k->mic=MIC_OFF; k->audio=AUDIO_STREAM; test_now=100;
    mic_task(k);
    assert(switches==1 && switch_on && k->mic==MIC_SWITCHING);
    mic_task(k); assert(switches==1); // never overlap mode changes
    k->mic=MIC_ON; test_now=200; observe_headset_jack(k,7);
    test_now=600; observe_headset_jack(k,7);
    mic_task(k);
    assert(switches==2 && !switch_on); // back to normal input after stable reports
    k->mic=MIC_ON; k->jack_known=false; test_now=1600;
    mic_task(k);
    assert(switches==3 && !switch_on && k->jack_known); // bounded on no response
    k->mic=MIC_OFF; k->audio=AUDIO_STREAM; s_mic_on=true;
    mic_task(k); assert(switches==4 && switch_on); // real recording still works
    puts("Headset jack state/probe: passed");
}
