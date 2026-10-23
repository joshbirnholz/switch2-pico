#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/src/web_api.c"
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
int main(void) {
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
