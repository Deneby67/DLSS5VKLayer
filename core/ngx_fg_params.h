#pragma once
#include "ngx_abi.h"
#include "logging.h"
#include <map>
#include <set>
#include <string>
#include <variant>
#include <type_traits>
#include <limits>
#include <cmath>

namespace dlssnr {
// MSVC reverses overload groups in a vtable; MinGW does not. Explicitly name
// each slot instead of compiling the SDK's overloaded declaration with MinGW.
// Unlike the legacy NR adapter, the public interface has 17 slots, with Reset
// at 0x80 and Get(uint64_t*) at 0x78. The latter MUST NOT clear the parameter map.
class FgParameters {
    using Value = std::variant<void*, unsigned long long, float, double, unsigned, int>;
    std::map<std::string, Value> values_;
    mutable std::set<std::string> missing_;
    template<class T,class V> static NVSDK_NGX_Result number(V value,T* output) {
        const long double n=value;
        if (!std::isfinite(n) || n < std::numeric_limits<T>::lowest() || n > std::numeric_limits<T>::max())
            return NVSDK_NGX_Result_FAIL_IncompatibleTypes;
        *output=static_cast<T>(value); return NVSDK_NGX_Result_Success;
    }
    template<class T> NVSDK_NGX_Result get(const char* key, T* output) const {
        if (!key || !output) return NVSDK_NGX_Result_FAIL_InvalidParameter;
        auto found=values_.find(key);
        if (found==values_.end()) {
            if (missing_.insert(key).second) Log("[fg-param-miss] %s",key);
            return NVSDK_NGX_Result_FAIL_InvalidParameter;
        }
        return std::visit([&](auto value) {
            using V=decltype(value);
            if constexpr (std::is_same_v<T,V>) { *output=value; return NVSDK_NGX_Result_Success; }
            else if constexpr (std::is_arithmetic_v<T> && std::is_arithmetic_v<V>) {
                return number(value,output);
            } else return NVSDK_NGX_Result_FAIL_IncompatibleTypes;
        },found->second);
    }
public:
    template<class T> void set(const char* key,T value) { if (key) values_[key]=value; }
    NVSDK_NGX_Parameter* abi() { return reinterpret_cast<NVSDK_NGX_Parameter*>(this); }
    virtual void setPointer(const char* k,void* v) { set(k,v); }                         // 0
    virtual void setD3D12(const char* k,void* v) { set(k,v); }                           // 1
    virtual void setD3D11(const char* k,void* v) { set(k,v); }                           // 2
    virtual void setInt(const char* k,int v) { set(k,v); }                              // 3
    virtual void setUint(const char* k,unsigned v) { set(k,v); }                        // 4
    virtual void setDouble(const char* k,double v) { set(k,v); }                        // 5
    virtual void setFloat(const char* k,float v) { set(k,v); }                          // 6
    virtual void setUll(const char* k,unsigned long long v) { set(k,v); }               // 7
    virtual NVSDK_NGX_Result getPointer(const char* k,void** v) const { return get(k,v); } // 8
    virtual NVSDK_NGX_Result getD3D12(const char* k,void** v) const { return get(k,v); } // 9
    virtual NVSDK_NGX_Result getD3D11(const char* k,void** v) const { return get(k,v); } // 10
    virtual NVSDK_NGX_Result getInt(const char* k,int* v) const { return get(k,v); }     // 11
    virtual NVSDK_NGX_Result getUint(const char* k,unsigned* v) const { return get(k,v); } // 12
    virtual NVSDK_NGX_Result getDouble(const char* k,double* v) const { return get(k,v); } // 13
    virtual NVSDK_NGX_Result getFloat(const char* k,float* v) const { return get(k,v); } // 14
    virtual NVSDK_NGX_Result getUll(const char* k,unsigned long long* v) const { return get(k,v); } // 15
    virtual void reset() { values_.clear(); }                                         // 16
};
}
