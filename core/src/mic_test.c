#include "mic_test.h"
#include <string.h>

const char *mic_test_phase_name(mic_test_phase_t phase) {
    static const char *const names[] = {"idle", "ready", "preparing", "recording", "playback", "done", "error"};
    return phase <= MIC_TEST_ERROR ? names[phase] : "error";
}

#if S2_HEADSET_MIC
#include <stdatomic.h>
// Five seconds of 20 ms CELT frames, with room for delivery jitter. Keeping
// compressed frames costs 26 KB rather than 480 KB of PCM.
#define CAPACITY 260u
static uint8_t s_audio[CAPACITY][S2_MIC_FRAME_BYTES];
static atomic_flag s_lock = ATOMIC_FLAG_INIT;
static mic_test_status_t s;
static uint32_t s_started, s_lease, s_last_frame, s_level_at, s_play_end;
static unsigned s_count, s_play;
static bool s_play_draining;
static uint32_t s_next_session;
static void lock(void) { while (atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire)) {} }
static void unlock(void) { atomic_flag_clear_explicit(&s_lock, memory_order_release); }
static bool active(void) { return s.phase >= MIC_TEST_READY && s.phase <= MIC_TEST_PLAYBACK; }
static void finish(mic_test_phase_t phase, const char *error) {
    s.phase=phase; s.error=error; s.peak=0;
    memset(s_audio,0,sizeof s_audio); s_count=s_play=0;
}
bool mic_test_supported(void) { return true; }
bool mic_test_active(void) { lock(); bool v=active(); unlock(); return v; }
bool mic_test_input(void) {
    lock(); bool v=s.phase>=MIC_TEST_READY && s.phase<=MIC_TEST_RECORDING; unlock(); return v;
}
uint32_t mic_test_open(uint32_t now) {
    lock();
    if (active()) { unlock(); return 0; }
    memset(&s,0,sizeof s);
    s.session=++s_next_session;
    if (!s.session) s.session=++s_next_session;
    s.phase=MIC_TEST_READY; s.error="";
    s_started=now; s_lease=now; s_last_frame=now; s_level_at=now;
    s_count=s_play=0; s_play_draining=false;
    uint32_t id=s.session; unlock(); return id;
}
bool mic_test_record(uint32_t session, uint32_t now) {
    lock();
    bool ok=session && session==s.session && s.phase==MIC_TEST_READY;
    if(ok) {s.phase=MIC_TEST_PREPARING; s_started=now; s_lease=now; s_count=0;}
    unlock(); return ok;
}
bool mic_test_cancel(uint32_t session) {
    lock(); bool ok=session && session==s.session;
    if(ok) finish(MIC_TEST_IDLE,"");
    unlock(); return ok;
}
void mic_test_status(uint32_t session, uint32_t now, mic_test_status_t *out) {
    lock();
    if(session && session==s.session && active()) s_lease=now;
    *out=s;
    out->elapsed_ms=0; out->remaining_ms=0;
    if(s.phase==MIC_TEST_RECORDING) {
        out->elapsed_ms=now-s_started;
        if(out->elapsed_ms>5000) out->elapsed_ms=5000;
        out->remaining_ms=5000-out->elapsed_ms;
    } else if(s.phase==MIC_TEST_PLAYBACK) {
        out->elapsed_ms=s_play*20;
        out->remaining_ms=(s_count-s_play)*20;
    }
    if(now-s_level_at>250) out->peak=0;
    unlock();
}
void mic_test_tick(uint32_t now, uint8_t jack, bool usb_busy) {
    lock();
    if(!active()) { unlock(); return; }
    if(!(jack & 1)) finish(MIC_TEST_ERROR,"The headset mic was disconnected. Plug it back in and try again.");
    else if(usb_busy) finish(MIC_TEST_ERROR,"Another app opened dongle audio. Close playback and recording, then try again.");
    else if(now-s_lease>5000) finish(MIC_TEST_ERROR,"The microphone test lost contact with the page. Please try again.");
    else if(s.phase!=MIC_TEST_PLAYBACK && now-s_last_frame>3000)
        finish(MIC_TEST_ERROR,"No microphone audio arrived. Check the headset and try again.");
    else if(s.phase==MIC_TEST_PREPARING && now-s_started>=600) {
        if (jack & 2) {s.phase=MIC_TEST_RECORDING; s_started=now; s_count=0;}
        else if (now-s_started>=3000)
            finish(MIC_TEST_ERROR,"No microphone was detected. Check that the headset plug is fully inserted and try again.");
    } else if(s.phase==MIC_TEST_RECORDING && now-s_started>=5000) {
        if(s_count<200) finish(MIC_TEST_ERROR,"The recording was interrupted. Move the controller closer and try again.");
        else {s.phase=MIC_TEST_PLAYBACK; s_started=now; s_play=0; s_play_draining=false; s.peak=0;}
    } else if(s.phase==MIC_TEST_PLAYBACK && s_play_draining && now-s_play_end>=300)
        finish(MIC_TEST_DONE,"");
    unlock();
}
void mic_test_frame(const uint8_t *frame,size_t len,uint32_t now) {
    if(len!=S2_MIC_FRAME_BYTES) return;
    lock();
    if(s.phase>=MIC_TEST_READY && s.phase<=MIC_TEST_RECORDING) {
        s_last_frame=now;
        if(s.phase==MIC_TEST_RECORDING && now-s_started<5000 && s_count<CAPACITY)
            memcpy(s_audio[s_count++],frame,len);
    }
    unlock();
}
bool mic_test_play_frame(uint8_t frame[S2_MIC_FRAME_BYTES],uint32_t *session,uint32_t now) {
    lock(); bool ok=s.phase==MIC_TEST_PLAYBACK && s_play<s_count;
    if(ok) {
        memcpy(frame,s_audio[s_play++],S2_MIC_FRAME_BYTES); *session=s.session;
        if(s_play==s_count) {s_play_draining=true; s_play_end=now;}
    }
    unlock(); return ok;
}
void mic_test_level(uint16_t peak,uint32_t session,uint32_t now) {
    lock();
    if(active() && (!session || session==s.session)) {s.peak=peak; s_level_at=now;}
    unlock();
}
void mic_test_fail(uint32_t session,const char *error) {
    lock(); if(s.session==session && active()) finish(MIC_TEST_ERROR,error); unlock();
}
#else
bool mic_test_supported(void) {return false;}
bool mic_test_active(void) {return false;}
bool mic_test_input(void) {return false;}
uint32_t mic_test_open(uint32_t now) {(void)now;return 0;}
bool mic_test_record(uint32_t session,uint32_t now) {(void)session;(void)now;return false;}
bool mic_test_cancel(uint32_t session) {(void)session;return false;}
void mic_test_status(uint32_t session,uint32_t now,mic_test_status_t *out) {
    (void)session;(void)now;memset(out,0,sizeof *out);out->error="Microphone testing is not supported by this firmware.";
}
void mic_test_tick(uint32_t now,uint8_t jack,bool usb_busy) {(void)now;(void)jack;(void)usb_busy;}
void mic_test_frame(const uint8_t *frame,size_t len,uint32_t now) {(void)frame;(void)len;(void)now;}
bool mic_test_play_frame(uint8_t frame[S2_MIC_FRAME_BYTES],uint32_t *session,uint32_t now) {(void)frame;(void)session;(void)now;return false;}
void mic_test_level(uint16_t peak,uint32_t session,uint32_t now) {(void)peak;(void)session;(void)now;}
void mic_test_fail(uint32_t session,const char *error) {(void)session;(void)error;}
#endif
