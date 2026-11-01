#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/src/web_api.c"
settings_t g_settings;
static uint32_t now;
static bool host_busy;
static uint8_t jack=3;
uint32_t platform_millis(void) {return now;}
bool platform_audio_active(void) {return host_busy || mic_test_active();}
bool platform_has_headset_audio(void) {return true;}
usb_mode_t usb_mode_active(void) {return USB_MODE_SINPUT;}
uint8_t s2_link_headset_jack(void) {return jack;}
static http_response_t request(const char *query,bool post) {
    http_request_t req={.query=query};http_response_t r={0};api_mic_test(&req,&r,post);return r;
}
static void release(http_response_t *r) {if(r->body_owned)free((void *)r->body);}
static void profile_compatibility(void) {
    char form[256]; size_t pos=0;
    pos += snprintf(form+pos,sizeof form-pos,"3,6,95,0,120,1,100,");
    for(int i=0;i<IN_COUNT;i++) pos+=snprintf(form+pos,sizeof form-pos,"0,");
    char *suffix=form+pos;
    profile_t *old=&g_settings.prof[0].p[0], parsed;
    old->used=1;old->rumble_style=1;old->impulse_strength=130;old->impulse_frequency=7;
    s_parse_type=0;s_parse_index=0;
    strcpy(suffix,"m0,100,0,Old client");
    assert(parse_profile(form,&parsed));
    assert(parsed.rumble_style==1 && parsed.impulse_strength==130 && !strcmp(parsed.name,"Old client"));
    strcpy(suffix,"m0,100,0,r3,5,2,80,9,Audio profile");
    assert(parse_profile(form,&parsed));
    assert(parsed.rumble_style==3 && parsed.rumble_frequency==5 && parsed.impulse_flags==2);
    assert(parsed.impulse_strength==80 && parsed.impulse_frequency==9 && !strcmp(parsed.name,"Audio profile"));
    strcpy(suffix,"m0,100,0,racing");
    assert(parse_profile(form,&parsed) && !strcmp(parsed.name,"racing") && parsed.rumble_style==1);
}
int main(void) {
    profile_compatibility();
    host_busy=true;http_response_t r=request("do=open",true);
    assert(r.status==409 && !mic_test_active());release(&r);
    host_busy=false;jack=0;r=request("do=open",true);
    assert(r.status==409 && !mic_test_active());release(&r);
    jack=1;r=request("do=open",true); // provisional mic status must not reject a connected headset
    assert(r.status==200 && strstr((const char *)r.body,"\"phase\":\"ready\""));release(&r);
    mic_test_status_t s;mic_test_status(0,now,&s);
    char query[80];snprintf(query,sizeof query,"do=record&session=%lu",(unsigned long)s.session);
    r=request(query,true);assert(r.status==200);release(&r);
    r=request("do=cancel&session=99999",true);assert(r.status==409 && mic_test_active());release(&r);
    snprintf(query,sizeof query,"do=cancel&session=%lu",(unsigned long)s.session);
    r=request(query,true);assert(r.status==200 && !mic_test_active());release(&r);
    r=request("do=bad",true);assert(r.status==400);release(&r);
    puts("Microphone HTTP/WebUSB API: passed");
}
