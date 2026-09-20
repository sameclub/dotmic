#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

// Independent of Arduino so the actual firmware DSP can be tested on a PC.
class Spectrum {
public:
    static constexpr int Size = 1024;
    static constexpr int Bands = 24;
    static constexpr float SampleRate = 48000.0f;
    // Unique FFT bins; approximately logarithmic, 93.75 Hz through 12 kHz.
    inline static constexpr int Edges[Bands+1] = {
        2,3,4,5,6,7,8,10,12,15,18,22,27,33,40,49,60,73,89,109,133,162,197,239,257
    };

    Spectrum() {
        for(int i=0;i<Size;++i) {
            window[i]=0.5f-0.5f*std::cos(2.0f*Pi*i/(Size-1));
            windowSum+=window[i];
        }
        for(int i=0;i<Size/2;++i) {
            twiddleReal[i]=std::cos(-2.0f*Pi*i/Size);
            twiddleImag[i]=std::sin(-2.0f*Pi*i/Size);
        }
    }

    void analyze(const int16_t *pcm, float *db) {
        float mean=0;
        for(int i=0;i<Size;++i) mean+=pcm[i]/32768.0f;
        mean/=Size;
        for(int i=0;i<Size;++i) {
            real[i]=(pcm[i]/32768.0f-mean)*window[i];
            imag[i]=0;
        }
        for(int i=1,j=0;i<Size;++i) {
            int bit=Size>>1;
            for(;j & bit;bit>>=1) j^=bit;
            j^=bit;
            if(i<j) { std::swap(real[i],real[j]); std::swap(imag[i],imag[j]); }
        }
        for(int len=2;len<=Size;len<<=1) {
            int half=len/2, step=Size/len;
            for(int base=0;base<Size;base+=len) for(int j=0;j<half;++j) {
                int a=base+j, b=a+half, t=j*step;
                float r=real[b]*twiddleReal[t]-imag[b]*twiddleImag[t];
                float q=real[b]*twiddleImag[t]+imag[b]*twiddleReal[t];
                real[b]=real[a]-r; imag[b]=imag[a]-q;
                real[a]+=r; imag[a]+=q;
            }
        }
        for(int band=0;band<Bands;++band) {
            float power=0;
            // Peak magnitude avoids making wider high-frequency bands louder
            // simply because they contain more bins. This is a visual spectrum.
            for(int bin=Edges[band];bin<Edges[band+1];++bin)
                power=std::max(power,real[bin]*real[bin]+imag[bin]*imag[bin]);
            float amplitude=2.0f*std::sqrt(power)/windowSum;
            db[band]=20.0f*std::log10(std::max(amplitude,1.0e-6f));
        }
    }

    static float height(float db) {
        // Fixed scale: no automatic gain that makes silence look loud.
        return std::max(0.0f,std::min(12.0f,(db+66.0f)*12.0f/60.0f));
    }
private:
    static constexpr float Pi=3.14159265358979323846f;
    float real[Size]={},imag[Size]={},window[Size]={};
    float twiddleReal[Size/2]={},twiddleImag[Size/2]={};
    float windowSum=0;
};

// Display-only smoothing. Does not alter the PCM sent to the computer.
class SpectrumEnvelope {
public:
    static constexpr int Columns=32;
    void reset() {
        for(int i=0;i<Spectrum::Bands;++i) { target[i]=0; current[i]=0; }
    }
    void silence() { for(float &v:target) v=0; }
    void accept(const float *db) {
        constexpr float weights[5]={1,2,3,2,1};
        for(int i=0;i<Spectrum::Bands;++i) {
            target[i]=0;
            for(int j=-2;j<=2;++j) {
                int neighbor=std::max(0,std::min(Spectrum::Bands-1,i+j));
                target[i]+=weights[j+2]*Spectrum::height(db[neighbor])/9.0f;
            }
        }
    }
    void step(float milliseconds) {
        milliseconds=std::max(0.0f,std::min(250.0f,milliseconds));
        for(int i=0;i<Spectrum::Bands;++i) {
            float tau=target[i]>current[i]?60.0f:180.0f;
            current[i]+=(target[i]-current[i])*(1.0f-std::exp(-milliseconds/tau));
        }
    }
    float column(int columnIndex) const {
        float position=std::max(0,std::min(Columns-1,columnIndex))*(Spectrum::Bands-1.0f)/(Columns-1);
        int left=static_cast<int>(position),right=std::min(left+1,Spectrum::Bands-1);
        float t=position-left;
        t=t*t*(3.0f-2.0f*t); // continuous, no overshoot between adjacent bands
        return current[left]+(current[right]-current[left])*t;
    }
private:
    float target[Spectrum::Bands]={},current[Spectrum::Bands]={};
};
