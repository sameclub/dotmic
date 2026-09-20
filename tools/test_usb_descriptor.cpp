#include <cassert>
#include <cstdio>
#include "tusb.h"
#include "class/audio/audio.h"
#include "device/usbd.h"
#include "../lib/DotMicAudio/src/DotMicAudioDescriptors.h"

int main() {
    // Same macro consumed by the firmware, with CDC occupying interfaces 0/1.
    const uint8_t descriptor[]={TUD_AUDIO10_MICROPHONE_DESCRIPTOR(2,4,0x83,48000,1,2,16,48000)};
    assert(sizeof(descriptor)==TUD_AUDIO10_MICROPHONE_DESC_LEN(1));
    size_t offset=0; int iad=0,ac=0,as=0,ep=0;
    while(offset<sizeof(descriptor)) {
        const uint8_t *d=descriptor+offset;
        assert(d[0]>=2 && offset+d[0]<=sizeof(descriptor));
        if(d[1]==0x0B) {
            ++iad; assert(offset==0 && d[0]==8 && d[2]==2 && d[3]==2);
            assert(d[4]==1 && d[5]==0 && d[6]==0);
        }
        if(d[1]==4) {
            if(d[2]==2) { ++ac; assert(d[5]==1 && d[6]==1 && d[3]==0); }
            else { ++as; assert(d[2]==3 && d[5]==1 && d[6]==2); }
        }
        if(d[1]==0x24 && d[2]==1 && ac==1 && as==0) {
            assert(d[7]==1 && d[8]==3); // AC collection owns AS interface 3
            unsigned total=d[5]|(d[6]<<8);
            assert(total==9+12+9); // header + input terminal + output terminal
        }
        if(d[1]==5) { ++ep; assert(d[2]==0x83 && (d[3]&3)==1 && d[6]==1); }
        offset+=d[0];
    }
    assert(iad==1 && ac==1 && as==2 && ep==1);
    std::printf("PASS: %zu-byte UAC1 microphone, IAD groups AC2/AS3, valid lengths and IN endpoint\n",sizeof(descriptor));
}
