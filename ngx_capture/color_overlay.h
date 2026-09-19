#pragma once
#include "params.h"
#include <cstring>
namespace ngx_capture {
// Public MSVC NGX 17-slot ABI. Never changes the application's parameter block.
// Forward every method and only replace a successful Color pointer lookup.
class ColorOverlay {
    const void* original;
    void* color;
    template<class T> void set(unsigned slot,const char* key,T value) {
        auto table=*static_cast<void* const* const*>(original);
        reinterpret_cast<void(__cdecl*)(const void*,const char*,T)>(table[slot])(original,key,value);
    }
    template<class T> Result get(unsigned slot,const char* key,T* value) const {
        auto table=*static_cast<void* const* const*>(original);
        return reinterpret_cast<Result(__cdecl*)(const void*,const char*,T*)>(table[slot])(original,key,value);
    }
public:
    ColorOverlay(const void* p,void* replacement):original(p),color(replacement){}
    virtual void s0(const char* k,void* v){set(0,k,v);}
    virtual void s1(const char* k,void* v){set(1,k,v);}
    virtual void s2(const char* k,void* v){set(2,k,v);}
    virtual void s3(const char* k,int v){set(3,k,v);}
    virtual void s4(const char* k,unsigned v){set(4,k,v);}
    virtual void s5(const char* k,double v){set(5,k,v);}
    virtual void s6(const char* k,float v){set(6,k,v);}
    virtual void s7(const char* k,unsigned long long v){set(7,k,v);}
    virtual Result g8(const char* k,void** v)const{
        auto r=get(8,k,v);
        if(r==Success && k && v && (!strcmp(k,"Color") || !strcmp(k,"#\x1e"))) *v=color;
        return r;
    }
    virtual Result g9(const char* k,void** v)const{return get(9,k,v);}
    virtual Result g10(const char* k,void** v)const{return get(10,k,v);}
    virtual Result g11(const char* k,int* v)const{return get(11,k,v);}
    virtual Result g12(const char* k,unsigned* v)const{return get(12,k,v);}
    virtual Result g13(const char* k,double* v)const{return get(13,k,v);}
    virtual Result g14(const char* k,float* v)const{return get(14,k,v);}
    virtual Result g15(const char* k,unsigned long long* v)const{return get(15,k,v);}
    virtual void reset(){
        auto table=*static_cast<void* const* const*>(original);
        reinterpret_cast<void(__cdecl*)(const void*)>(table[16])(original);
    }
};
}
