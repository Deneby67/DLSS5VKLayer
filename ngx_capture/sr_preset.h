#pragma once
#include "params.h"
#include <cstring>

namespace ngx_capture {
inline bool SrPresetSupported(unsigned p) {
    return p==0 || p==5 || p==6 || p==10 || p==11 || p==12 || p==13;
}
inline bool SrPresetKey(const char* k) {
    if(!k)return false;
    for(const char* key:{"DLSS.Hint.Render.Preset.DLAA","DLSS.Hint.Render.Preset.Quality",
        "DLSS.Hint.Render.Preset.Balanced","DLSS.Hint.Render.Preset.Performance",
        "DLSS.Hint.Render.Preset.UltraPerformance","DLSS.Hint.Render.Preset.UltraQuality"})
        if(!strcmp(k,key))return true;
    return false;
}
// Public 17-slot NGX ABI, with only preset reads overlaid. Never overwrite the
// engine's parameter block or its quality/extent/exposure/motion contract.
class SrPresetOverlay {
    void* original;
    unsigned preset;
    template<class T> void set(unsigned slot,const char* key,T value) {
        auto table=*static_cast<void***>(original);
        reinterpret_cast<void(__cdecl*)(void*,const char*,T)>(table[slot])(original,key,value);
    }
    template<class T> Result get(unsigned slot,const char* key,T* value) const {
        auto table=*static_cast<void***>(original);
        return reinterpret_cast<Result(__cdecl*)(void*,const char*,T*)>(table[slot])(original,key,value);
    }
    template<class T> bool override(const char* key,T* value) const {
        if(!preset || !SrPresetKey(key) || !value)return false;
        *value=T(preset);++reads;return true;
    }
public:
    mutable unsigned reads=0;
    SrPresetOverlay(void* p,unsigned selected):original(p),preset(selected){}
    virtual void s0(const char* k,void* v){set(0,k,v);}
    virtual void s1(const char* k,void* v){set(1,k,v);}
    virtual void s2(const char* k,void* v){set(2,k,v);}
    virtual void s3(const char* k,int v){set(3,k,v);}
    virtual void s4(const char* k,unsigned v){set(4,k,v);}
    virtual void s5(const char* k,double v){set(5,k,v);}
    virtual void s6(const char* k,float v){set(6,k,v);}
    virtual void s7(const char* k,unsigned long long v){set(7,k,v);}
    virtual Result g8(const char* k,void** v)const{return get(8,k,v);}
    virtual Result g9(const char* k,void** v)const{return get(9,k,v);}
    virtual Result g10(const char* k,void** v)const{return get(10,k,v);}
    virtual Result g11(const char* k,int* v)const{return override(k,v)?Success:get(11,k,v);}
    virtual Result g12(const char* k,unsigned* v)const{return override(k,v)?Success:get(12,k,v);}
    virtual Result g13(const char* k,double* v)const{return get(13,k,v);}
    virtual Result g14(const char* k,float* v)const{return get(14,k,v);}
    virtual Result g15(const char* k,unsigned long long* v)const{return get(15,k,v);}
    virtual void reset(){
        auto table=*static_cast<void***>(original);
        reinterpret_cast<void(__cdecl*)(void*)>(table[16])(original);
    }
};
}
