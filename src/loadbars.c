// Black side bars on loading screens. Without the 16:9 letterbox the loading screen still draws at 16:9 and
// the sides keep the last world frame, so this clears them in the back buffer right before Present.
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <string.h>
#include "ember.h"

#define FRAMES 4
// Present calls after the last loading screen update that still get bars
#define HOLD 8

static volatile LONG64 updates;
static LONG64 seen;
static int idle = HOLD;
static uint8_t *thunk;

static IDXGISwapChain *swapchain;
static ID3D12CommandQueue *queue;
static ID3D12Device *device;
static ID3D12DescriptorHeap *heap;
static ID3D12CommandAllocator *allocs[FRAMES];
static UINT64 alloc_done[FRAMES];
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence;
static UINT64 fence_value;
static HANDLE fence_event;
static bool broken;

static HRESULT(WINAPI *o_factory)(REFIID, void **);
static HRESULT(WINAPI *o_factory1)(REFIID, void **);
static HRESULT(STDMETHODCALLTYPE *o_create)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
static HRESULT(STDMETHODCALLTYPE *o_create_hwnd)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                                 const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *,
                                                 IDXGISwapChain1 **);
static HRESULT(STDMETHODCALLTYPE *o_present)(IDXGISwapChain *, UINT, UINT);

static void vtable_hook(void *object, size_t offset, void *hook, void **orig)
{
    void **slot = (void **)((uint8_t *)*(void **)object + offset);
    if (*slot == hook)
        return;
    *orig = *slot;
    patch_code(slot, &hook, sizeof hook);
}

static bool init(void)
{
    if (FAILED(ID3D12CommandQueue_GetDevice(queue, &IID_ID3D12Device, (void **)&device)))
        return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(ID3D12Device_CreateDescriptorHeap(device, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap)))
        return false;
    for (int i = 0; i < FRAMES; i++)
        if (FAILED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       &IID_ID3D12CommandAllocator, (void **)&allocs[i])))
            return false;
    if (FAILED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocs[0], NULL,
                                              &IID_ID3D12GraphicsCommandList, (void **)&list)))
        return false;
    ID3D12GraphicsCommandList_Close(list);
    if (FAILED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence)))
        return false;
    fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    return true;
}

static void barrier(ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

static void clear_sides(IDXGISwapChain *sc)
{
    IDXGISwapChain3 *sc3;
    if (FAILED(IDXGISwapChain_QueryInterface(sc, &IID_IDXGISwapChain3, (void **)&sc3)))
        return;
    DXGI_SWAP_CHAIN_DESC1 d;
    UINT i = IDXGISwapChain3_GetCurrentBackBufferIndex(sc3);
    ID3D12Resource *buf = NULL;
    if (SUCCEEDED(IDXGISwapChain3_GetDesc1(sc3, &d)) && (UINT64)d.Width * 9 > (UINT64)d.Height * 16 &&
        SUCCEEDED(IDXGISwapChain3_GetBuffer(sc3, i, &IID_ID3D12Resource, (void **)&buf))) {
        LONG bar = (LONG)(d.Width - (UINT64)d.Height * 16 / 9) / 2;
        D3D12_RECT rects[2] = {{0, 0, bar, d.Height}, {d.Width - bar, 0, d.Width, d.Height}};
        int f = i % FRAMES;
        if (ID3D12Fence_GetCompletedValue(fence) < alloc_done[f]) {
            ID3D12Fence_SetEventOnCompletion(fence, alloc_done[f], fence_event);
            WaitForSingleObject(fence_event, 1000);
        }
        ID3D12CommandAllocator_Reset(allocs[f]);
        ID3D12GraphicsCommandList_Reset(list, allocs[f], NULL);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
        ID3D12Device_CreateRenderTargetView(device, buf, NULL, rtv);
        barrier(buf, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float black[4] = {0, 0, 0, 1};
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, black, 2, rects);
        barrier(buf, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        ID3D12GraphicsCommandList_Close(list);
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
        ID3D12CommandQueue_Signal(queue, fence, alloc_done[f] = ++fence_value);
        ID3D12Resource_Release(buf);
    }
    IDXGISwapChain3_Release(sc3);
}

static HRESULT STDMETHODCALLTYPE h_present(IDXGISwapChain *sc, UINT sync, UINT flags)
{
    LONG64 n = updates;
    if (n != seen) {
        seen = n;
        idle = 0;
    } else if (idle < HOLD) {
        idle++;
    }
    if (idle < HOLD && sc == swapchain && queue && !broken && !(flags & DXGI_PRESENT_TEST)) {
        if (!device && !init()) {
            broken = true;
            log_line(LOG_ERROR, "loadbars could not set up D3D12 objects");
        } else {
            clear_sides(sc);
        }
    }
    return o_present(sc, sync, flags);
}

static void on_swapchain(IUnknown *dev, IDXGISwapChain *sc)
{
    ID3D12CommandQueue *q;
    if (FAILED(IUnknown_QueryInterface(dev, &IID_ID3D12CommandQueue, (void **)&q)))
        return;
    if (queue && queue != q)
        log_line(LOG_WARN, "loadbars swapchain moved to another queue");
    queue = q; // keeps the reference, the queue outlives every swapchain on it
    swapchain = sc;
    vtable_hook(sc, offsetof(IDXGISwapChainVtbl, Present), h_present, (void **)&o_present);
    log_line(LOG_INFO, "loadbars hooked swapchain %p", sc);
}

static HRESULT STDMETHODCALLTYPE h_create(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *desc,
                                          IDXGISwapChain **out)
{
    HRESULT hr = o_create(f, dev, desc, out);
    if (SUCCEEDED(hr))
        on_swapchain(dev, *out);
    return hr;
}

static HRESULT STDMETHODCALLTYPE h_create_hwnd(IDXGIFactory2 *f, IUnknown *dev, HWND w,
                                               const DXGI_SWAP_CHAIN_DESC1 *desc,
                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fs, IDXGIOutput *o,
                                               IDXGISwapChain1 **out)
{
    HRESULT hr = o_create_hwnd(f, dev, w, desc, fs, o, out);
    if (SUCCEEDED(hr))
        on_swapchain(dev, (IDXGISwapChain *)*out);
    return hr;
}

static void hook_factory(void *factory)
{
    IDXGIFactory2 *f2;
    vtable_hook(factory, offsetof(IDXGIFactoryVtbl, CreateSwapChain), h_create, (void **)&o_create);
    if (SUCCEEDED(IUnknown_QueryInterface((IUnknown *)factory, &IID_IDXGIFactory2, (void **)&f2))) {
        vtable_hook(f2, offsetof(IDXGIFactory2Vtbl, CreateSwapChainForHwnd), h_create_hwnd, (void **)&o_create_hwnd);
        IDXGIFactory2_Release(f2);
    }
}

static HRESULT WINAPI h_factory(REFIID riid, void **out)
{
    HRESULT hr = o_factory(riid, out);
    if (SUCCEEDED(hr))
        hook_factory(*out);
    return hr;
}

static HRESULT WINAPI h_factory1(REFIID riid, void **out)
{
    HRESULT hr = o_factory1(riid, out);
    if (SUCCEEDED(hr))
        hook_factory(*out);
    return hr;
}

static uint8_t *find(uint8_t *from, uint8_t *to, const void *what, size_t n, size_t step)
{
    for (; from + n <= to; from += step)
        if (!memcmp(from, what, n))
            return from;
    return NULL;
}

// Finds a class's vtable from its RTTI type name: type descriptor, then complete object locator, then vtable.
// They sit in different data sections, so each step searches all of them.
static void **find_vtable(const char *type_name)
{
    uint8_t *base = (uint8_t *)GetModuleHandleA(NULL), *lo[16], *hi[16];
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (int k = 0; k < nt->FileHeader.NumberOfSections && n < 16; k++, s++)
        if (!(s->Characteristics & IMAGE_SCN_MEM_EXECUTE) && s->Characteristics & IMAGE_SCN_MEM_READ) {
            lo[n] = base + s->VirtualAddress;
            hi[n++] = base + s->VirtualAddress + s->Misc.VirtualSize;
        }
    uint8_t *name = NULL;
    for (int i = 0; i < n && !name; i++)
        name = find(lo[i], hi[i], type_name, strlen(type_name) + 1, 1);
    if (!name)
        return NULL;
    uint32_t td = (uint32_t)(name - 0x10 - base);
    for (int i = 0; i < n; i++)
        for (uint8_t *p = lo[i] + 12; (p = find(p, hi[i], &td, 4, 4)); p += 4) {
            uint32_t *col = (uint32_t *)(p - 12);
            if (col[0] != 1 || col[1] != 0)
                continue;
            uintptr_t colva = (uintptr_t)col;
            for (int j = 0; j < n; j++) {
                uint8_t *ref = find(lo[j], hi[j], &colva, 8, 8);
                if (ref)
                    return (void **)(ref + 8);
            }
        }
    return NULL;
}

static DWORD WINAPI hook_loading_screen(LPVOID arg)
{
    (void)arg;
    void **vt = find_vtable(".?AVLoadingScreen@CS@@");
    if (!vt) {
        log_line(LOG_ERROR, "loadbars LoadingScreen vtable not found");
        return 0;
    }
    // slot 2 runs every frame while the loading screen is up: lock inc [updates], then jump to the original
    uint8_t code[] = {0x50, 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xF0, 0x48, 0xFF, 0x00, 0x58,
                      0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uintptr_t counter = (uintptr_t)&updates;
    memcpy(code + 3, &counter, 8);
    memcpy(code + 22, &vt[2], 8);
    thunk = VirtualAlloc(NULL, sizeof code, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(thunk, code, sizeof code);
    patch_code(&vt[2], &thunk, sizeof thunk);
    log_line(LOG_INFO, "loadbars LoadingScreen vtable at %p", vt);
    return 0;
}

void loadbars_start(void)
{
    int n = iat_hook("dxgi.dll", "CreateDXGIFactory", h_factory, (void **)&o_factory) +
            iat_hook("dxgi.dll", "CreateDXGIFactory1", h_factory1, (void **)&o_factory1);
    if (!n) {
        log_line(LOG_ERROR, "loadbars dxgi imports not found");
        return;
    }
    CloseHandle(CreateThread(NULL, 0, hook_loading_screen, NULL, 0, NULL));
}
