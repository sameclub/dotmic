#include "../src/spectrum.h"
#include <cassert>
#include <cstdio>

static Spectrum fft;
static int16_t pcm[Spectrum::Size];
static float db[Spectrum::Bands];
static constexpr double Pi=3.14159265358979323846;

static void tone(int bin, float amplitude) {
    for(int i=0;i<Spectrum::Size;++i)
        pcm[i]=static_cast<int16_t>(amplitude*32767*std::sin(2*Pi*bin*i/Spectrum::Size));
}
static int bandFor(int bin) {
    for(int b=0;b<Spectrum::Bands;++b)
        if(bin>=Spectrum::Edges[b] && bin<Spectrum::Edges[b+1]) return b;
    return -1;
}
int main() {
    fft.analyze(pcm,db);
    for(float v:db) assert(std::isfinite(v) && Spectrum::height(v)==0);
    for(auto &sample:pcm) sample=12345;
    fft.analyze(pcm,db);
    for(float v:db) assert(Spectrum::height(v)==0); // DC rejection
    for(int bin:{5,40,200}) { // 234 Hz / 1875 Hz / 9375 Hz
        tone(bin,0.5f); fft.analyze(pcm,db);
        int peak=static_cast<int>(std::max_element(db,db+Spectrum::Bands)-db);
        assert(peak==bandFor(bin));
        assert(std::fabs(db[peak]+6.021f)<0.1f);
        std::printf("tone %.1f Hz -> band %d, %.2f dBFS: PASS\n",bin*48000.0/1024,peak,db[peak]);
    }
    tone(40,0.5f); fft.analyze(pcm,db); float loud=db[bandFor(40)];
    tone(40,0.25f); fft.analyze(pcm,db);
    assert(std::fabs(loud-db[bandFor(40)]-6.021f)<0.1f);
    for(int i=0;i<Spectrum::Size;++i) pcm[i]=static_cast<int16_t>(
        8000*(std::sin(2*Pi*5*i/Spectrum::Size)+std::sin(2*Pi*200*i/Spectrum::Size)));
    fft.analyze(pcm,db);
    assert(db[bandFor(5)]>-13 && db[bandFor(200)]>-13);
    assert(db[bandFor(40)]<-66); // tones at both ends do not invent a middle tone
    tone(40,0.0001f); fft.analyze(pcm,db);
    for(float v:db) assert(Spectrum::height(v)==0); // noise floor, no auto gain
    for(int i=0;i<Spectrum::Size;++i) pcm[i]=i%2?-32768:32767;
    fft.analyze(pcm,db);
    for(float v:db) assert(std::isfinite(v) && Spectrum::height(v)>=0 && Spectrum::height(v)<=12);
    std::puts("silence, DC, mixed tones, amplitude scaling, floor, bounds: PASS");
    SpectrumEnvelope envelope,twice;
    for(float &v:db) v=-120;
    envelope.accept(db); envelope.step(25);
    for(int i=0;i<SpectrumEnvelope::Columns;++i) assert(envelope.column(i)==0);
    for(float &v:db) v=-6;
    envelope.accept(db); twice.accept(db);
    envelope.step(50); twice.step(25); twice.step(25);
    for(int i=0;i<SpectrumEnvelope::Columns;++i) {
        assert(std::fabs(envelope.column(i)-twice.column(i))<0.0001f);
        assert(envelope.column(i)>0 && envelope.column(i)<12);
    }
    envelope.reset();
    for(float &v:db) v=-120;
    db[12]=-6;
    envelope.accept(db);
    for(int n=0;n<40;++n) envelope.step(25);
    float peak=0;
    for(int i=0;i<SpectrumEnvelope::Columns;++i) {
        float h=envelope.column(i); peak=std::max(peak,h);
        assert(h>=0 && h<=12);
        if(i) assert(std::fabs(h-envelope.column(i-1))<1.8f);
    }
    assert(peak>3 && envelope.column(0)==0 && envelope.column(SpectrumEnvelope::Columns-1)==0);
    constexpr int center=(SpectrumEnvelope::Columns-1)*12/23;
    float before=envelope.column(center);
    envelope.silence(); envelope.step(25);
    assert(envelope.column(center)>0 && envelope.column(center)<before);
    for(int n=0;n<100;++n) envelope.step(25);
    assert(envelope.column(center)<0.001f);
    envelope.reset();
    for(int i=0;i<SpectrumEnvelope::Columns;++i) assert(envelope.column(i)==0);
    std::puts("smooth contour, time-based transitions, silence decay and reset: PASS");
}
