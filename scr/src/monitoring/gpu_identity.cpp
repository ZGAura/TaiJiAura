#include <desktopball/monitoring/gpu_identity.hpp>
#include <winternl.h>
#include <d3dkmthk.h>
#include <vector>

namespace desktopball {
GpuIdentity queryGpuIdentity(LUID luid) {
    GpuIdentity identity;
    identity.luid=(static_cast<std::uint64_t>(static_cast<std::uint32_t>(luid.HighPart))<<32) | luid.LowPart;
    D3DKMT_OPENADAPTERFROMLUID opened{}; opened.AdapterLuid=luid;
    if (D3DKMTOpenAdapterFromLuid(&opened)<0) return identity;
    struct CloseAdapter {
        D3DKMT_HANDLE handle;
        ~CloseAdapter() { const D3DKMT_CLOSEADAPTER close{handle}; D3DKMTCloseAdapter(&close); }
    } close{opened.hAdapter};
    D3DKMT_ADAPTERTYPE type{};
    D3DKMT_QUERYADAPTERINFO query{opened.hAdapter,KMTQAITYPE_ADAPTERTYPE,&type,sizeof(type)};
    if (D3DKMTQueryAdapterInfo(&query)>=0) {
        identity.software=type.SoftwareDevice!=0;
        identity.indirectDisplay=type.IndirectDisplayDevice!=0;
    }
    std::vector<wchar_t> buffer(4096);
    UINT count=static_cast<UINT>(buffer.size());
    D3DKMT_QUERY_PHYSICAL_ADAPTER_PNP_KEY physical{0,D3DKMT_PNP_KEY_HARDWARE,buffer.data(),&count};
    query={opened.hAdapter,KMTQAITYPE_PHYSICALADAPTERPNPKEY,&physical,sizeof(physical)};
    auto status=D3DKMTQueryAdapterInfo(&query);
    if (status<0 && count>buffer.size() && count<=32768) {
        buffer.resize(count); physical.pDest=buffer.data();
        status=D3DKMTQueryAdapterInfo(&query);
    }
    if (status>=0 && count>0 && count<=buffer.size() && buffer[count-1]==L'\0') identity.physicalKey=buffer.data();
    return identity;
}
}
