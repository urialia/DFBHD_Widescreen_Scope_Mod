/*
 * DFBHD Widescreen Fix - d3d8.dll Proxy
 *
 * 동작 원리:
 *   게임 디렉토리에 이 DLL을 d3d8.dll로 놓으면
 *   게임이 시스템 d3d8.dll 대신 이걸 로드한다.
 *   Direct3DCreate8()을 후킹해서 IDirect3D8 래퍼를 반환하고,
 *   IDirect3DDevice8::DrawPrimitiveUP 을 가로채서
 *   조준경 쿼드의 버텍스 X좌표에 aspect ratio 보정을 적용한다.
 *
 * 조준경 원이 타원이 되는 이유:
 *   GMAT_OVR_CLAMPUV 방식으로 UV [0..1]x[0..1] 공간에서
 *   원형 알파 마스크를 렌더링하는데, 화면이 16:9면
 *   UV 1.0 = 1920px(가로) vs UV 1.0 = 1080px(세로)라서
 *   원이 가로로 늘어난 타원처럼 보인다.
 *
 * 보정 방법:
 *   조준경 쿼드의 X 버텍스 좌표를 화면 중앙 기준으로
 *   (4/3) / (현재종횡비) 배 축소.
 *   예) 16:9: 보정계수 = (4/3)/(16/9) = 0.75
 */

#define WIN32_LEAN_AND_MEAN
#define DIRECT3D_VERSION 0x0800
#include <windows.h>
#include <d3d8.h>
#include <cmath>
#include <cstdio>
#include <cstring>

// ─── stb_image: 단일 헤더 TGA 로더 ──────────────────────────────────────────
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO   // fopen 대신 Win32 파일 API 사용
#define STBI_NO_PNG
#define STBI_NO_JPEG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
// TGA만 활성화
#include "stb_image.h"

// ─── 시스템 d3d8.dll (lazy init) ─────────────────────────────────────────────
// DllMain 안에서 LoadLibrary 호출 금지 (Loader Lock 재진입 → AV 크래시)
// Direct3DCreate8 첫 호출 시점에 한 번만 로드한다.

static HMODULE g_hRealD3D8   = NULL;
typedef IDirect3D8* (WINAPI* PFN_Direct3DCreate8)(UINT);
static PFN_Direct3DCreate8 g_RealCreate8 = NULL;

static HMODULE g_hDll = NULL;   // 이 DLL 자신의 핸들 (PNG 경로 계산용)

// DLL 디렉토리에서 파일 경로 생성
static void GetDllDirPath(const char* filename, char* out, DWORD outSize)
{
    GetModuleFileNameA(g_hDll, out, outSize);
    char* slash = strrchr(out, '\\');
    if (slash) { ++slash; *slash = '\0'; }
    else        { out[0] = '\0'; }
    strncat(out, filename, outSize - strlen(out) - 1);
}

static bool InitRealD3D8()
{
    if (g_RealCreate8) return true;          // 이미 초기화됨

    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat(path, "\\d3d8.dll");

    g_hRealD3D8 = LoadLibraryA(path);       // ← DllMain 밖에서 호출하므로 안전
    if (!g_hRealD3D8) {
        MessageBoxA(NULL,
            "DFBHD Widescreen Fix:\n시스템 d3d8.dll 로드 실패.",
            "오류", MB_ICONERROR);
        return false;
    }

    g_RealCreate8 = (PFN_Direct3DCreate8)
        GetProcAddress(g_hRealD3D8, "Direct3DCreate8");
    return g_RealCreate8 != NULL;
}

// ─── 전역 상태 ───────────────────────────────────────────────────────────────

static DWORD* g_pGfScope    = NULL;
static bool   g_scopeFound  = false;
static float  g_screenW     = 1920.0f;
static float  g_screenH     = 1080.0f;
static float  g_correction  = (4.0f / 3.0f) / (16.0f / 9.0f);  // 0.75

static FILE*  g_log         = NULL;
#define LOG(fmt, ...) do{ if(g_log){ fprintf(g_log, fmt, ##__VA_ARGS__); fflush(g_log); } }while(0)

// ─── GF_SCOPE 주소 검증 ──────────────────────────────────────────────────────

static void FindGfScope()
{
    // DFBHD는 ASLR 없음, 고정 VA
    // 분석에서 확인: GF_SCOPE 전역변수 = 0x00A0201C
    const DWORD GF_SCOPE_ADDR = 0x00A0201C;

    // IsBadReadPtr으로 안전하게 접근 가능 여부 확인
    DWORD* p = reinterpret_cast<DWORD*>(GF_SCOPE_ADDR);
    if (!IsBadReadPtr(p, sizeof(DWORD))) {
        g_pGfScope  = p;
        g_scopeFound = true;
        LOG("[DFBHD-WS] GF_SCOPE 주소 확인: 0x%08lX\n", (unsigned long)GF_SCOPE_ADDR);
    } else {
        LOG("[DFBHD-WS] GF_SCOPE 접근 실패 (주소 재확인 필요)\n");
        // 폴백: 항상 보정 적용 (조준경 외 화면도 보정될 수 있음)
        g_scopeFound = false;
    }
}

static inline bool IsScopeActive()
{
    if (!g_scopeFound) return false;
    if (IsBadReadPtr(g_pGfScope, sizeof(DWORD))) return false;
    return (*g_pGfScope) != 0;
}

// ─── 보정 적용 ───────────────────────────────────────────────────────────────

static void CorrectQuadX(void* pVB, UINT vcount, UINT stride)
{
    const float cx = g_screenW * 0.5f;
    BYTE* p = reinterpret_cast<BYTE*>(pVB);
    for (UINT i = 0; i < vcount; ++i) {
        float* x = reinterpret_cast<float*>(p + i * stride);
        *x = cx + (*x - cx) * g_correction;
    }
}

// ─── 전체화면 조준경 쿼드 판별 ───────────────────────────────────────────────

static bool IsFullscreenScopeQuad(
    D3DPRIMITIVETYPE primType,
    UINT             primCount,
    const void*      pVB,
    UINT             stride,
    DWORD            fvf)
{
    // TRIANGLESTRIP(5) 또는 TRIANGLEFAN(6) 만 처리
    if (primType != D3DPT_TRIANGLESTRIP && primType != D3DPT_TRIANGLEFAN)
        return false;

    UINT vcount = primCount + 2;
    if (vcount < 3 || vcount > 6) return false;

    // RHW(변환 완료) 버텍스여야 함
    if (!(fvf & D3DFVF_XYZRHW)) return false;

    // ※ IsScopeActive() 제거:
    //   GF_SCOPE 메모리 주소 검증에 의존하지 않고,
    //   순수 기하학(화면 85% 이상 커버)으로만 조준경 쿼드를 판별.
    //   오탐 방지를 위해 임계값을 50% → 85%로 높임.
    const BYTE* p = reinterpret_cast<const BYTE*>(pVB);
    float minX =  1e9f, maxX = -1e9f;
    float minY =  1e9f, maxY = -1e9f;
    for (UINT i = 0; i < vcount; ++i) {
        const float* xy = reinterpret_cast<const float*>(p + i * stride);
        if (xy[0] < minX) minX = xy[0];
        if (xy[0] > maxX) maxX = xy[0];
        if (xy[1] < minY) minY = xy[1];
        if (xy[1] > maxY) maxY = xy[1];
    }
    if ((maxX - minX) < g_screenW * 0.85f) return false;
    if ((maxY - minY) < g_screenH * 0.85f) return false;

    return true;
}

// ─── 전방 선언 ───────────────────────────────────────────────────────────────

class ProxyDevice8;
class ProxyD3D8;

// ─── IDirect3DDevice8 프록시 ─────────────────────────────────────────────────

class ProxyDevice8 : public IDirect3DDevice8
{
public:
    ProxyDevice8(IDirect3DDevice8* real, UINT w, UINT h)
        : m_real(real), m_ref(1), m_fvf(0), m_gameVPSet(false)
    , m_capturedVB(NULL), m_capturedStride(0)
    , m_scopeVB(NULL), m_scopeVBSize(0), m_scopeVBDirty(true)
    , m_circleKnown(false), m_circleCX(0.0f), m_circleCY(0.0f), m_circleR(0.0f)
    , m_scopeDrawnThisFrame(false)
    , m_expectScopeQuadNext(false)
    , m_scopeOverlayTex(NULL), m_scopeTexLoaded(false), m_scopeOverlayDrawn(false)
    , m_mmPatchTex(NULL), m_mmPatchTexLoaded(false), m_mmPatchDrawn(false)
    {
        memset(&m_gameVP, 0, sizeof(m_gameVP));
        g_screenW    = (float)w;
        g_screenH    = (float)h;
        float aspect = g_screenW / g_screenH;
        g_correction = (4.0f / 3.0f) / aspect;
        // 4:3 화면이면 보정 불필요 (1.0)
        if (g_correction > 0.999f) g_correction = 1.0f;
        LOG("[DFBHD-WS] Device: %ux%u  aspect=%.4f  correction=%.4f\n",
            w, h, aspect, g_correction);
        FindGfScope();
    }

    // IUnknown
    HRESULT WINAPI QueryInterface(REFIID r, void** p) override { return m_real->QueryInterface(r,p); }
    ULONG   WINAPI AddRef()  override { m_real->AddRef(); return ++m_ref; }
    ULONG   WINAPI Release() override {
        // m_ref가 0이 되는 시점에만 m_real->Release() 및 소멸
        // m_real->Release()를 먼저 호출하면 m_real이 먼저 소멸될 위험
        ULONG ref = --m_ref;
        if (ref == 0) {
            m_real->Release();
            delete this;
        }
        return ref;
    }

    // ── 핵심 후킹 ────────────────────────────────────────────────────────────

    // FVF 추적 (D3D8에서 SetVertexShader로 FVF 설정)
    HRESULT WINAPI SetVertexShader(DWORD h) override {
        // 낮은 값(0xFFFF 이하)은 FVF 플래그, 큰 값은 셰이더 핸들
        if (h < 0x10000) m_fvf = h;
        return m_real->SetVertexShader(h);
    }

    // ── PNG 오버레이 텍스처 로드 (최초 1회) ─────────────────────────────────
    void EnsureScopeTexture()
    {
        if (m_scopeTexLoaded) return;
        m_scopeTexLoaded = true;

        // DLL 폴더에서 scope_overlay.png 읽기
        char path[MAX_PATH];
        GetDllDirPath("scope_overlay.tga", path, MAX_PATH);
        LOG("[DFBHD-WS] TGA 로드 시도: %s\n", path);

        // Win32 파일 API로 읽어서 메모리로 넘김 (STBI_NO_STDIO이므로)
        HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                                   NULL, OPEN_EXISTING, 0, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            LOG("[DFBHD-WS] scope_overlay.tga 없음 → 폴백 모드\n");
            return;
        }
        DWORD fileSize = GetFileSize(hFile, NULL);
        BYTE* fileBuf  = new BYTE[fileSize];
        DWORD read = 0;
        ReadFile(hFile, fileBuf, fileSize, &read, NULL);
        CloseHandle(hFile);

        // TGA는 기본 원점이 좌하단(Bottom-Left),
        // D3D8 텍스처는 좌상단(Top-Left) 기준 → 수직 뒤집기 필요
        stbi_set_flip_vertically_on_load(1);

        // stb_image로 TGA 디코딩 (RGBA 8bpc 강제)
        int w, h, ch;
        unsigned char* pixels = stbi_load_from_memory(fileBuf, (int)fileSize,
                                                       &w, &h, &ch, 4);
        delete[] fileBuf;
        if (!pixels) {
            LOG("[DFBHD-WS] TGA 디코딩 실패\n");
            return;
        }
        LOG("[DFBHD-WS] TGA 디코딩 성공: %dx%d 원본채널수=%d %s\n",
            w, h, ch,
            (ch == 4) ? "(알파 있음)" : "(알파 없음! 전체 불투명 처리됨)");

        // D3D8 텍스처 생성 (A8R8G8B8)
        HRESULT hr = m_real->CreateTexture(w, h, 1, 0,
                                           D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                           &m_scopeOverlayTex);
        if (FAILED(hr)) {
            LOG("[DFBHD-WS] TGA 텍스처 생성 실패\n");
            stbi_image_free(pixels);
            return;
        }

        // 픽셀 복사: RGBA(stb) → ARGB(D3D) 채널 스왑
        // stb_image가 채널 수에 관계없이 RGBA 4ch으로 반환하도록 요청했으므로
        // src[0]=R, src[1]=G, src[2]=B, src[3]=A 순서로 처리
        D3DLOCKED_RECT lr;
        m_scopeOverlayTex->LockRect(0, &lr, NULL, 0);
        for (int y = 0; y < h; ++y) {
            DWORD*         dst = reinterpret_cast<DWORD*>(
                                     static_cast<BYTE*>(lr.pBits) + y * lr.Pitch);
            unsigned char* src = pixels + y * w * 4;
            for (int x = 0; x < w; ++x, src += 4)
                dst[x] = (DWORD(src[3]) << 24)   // A
                        | (DWORD(src[0]) << 16)   // R
                        | (DWORD(src[1]) <<  8)   // G
                        |  DWORD(src[2]);          // B
        }
        m_scopeOverlayTex->UnlockRect(0);
        stbi_image_free(pixels);
        LOG("[DFBHD-WS] scope_overlay.tga 로드 완료: %dx%d\n", w, h);
    }

    // ── 미니맵 패치 TGA 로드 (기존 scope_overlay와 완전히 동일한 방식) ───────
    // 회색 선 등 가리고 싶은 부분만 불투명, 나머지는 투명으로 만든 TGA를
    // 조준경 활성 시 scope_overlay 위에 한 번 더 그려서 덮는다.
    void EnsureMinimapPatchTexture()
    {
        if (m_mmPatchTexLoaded) return;
        m_mmPatchTexLoaded = true;

        char path[MAX_PATH];
        GetDllDirPath("minimap_patch.tga", path, MAX_PATH);
        LOG("[DFBHD-WS] 미니맵 패치 TGA 로드 시도: %s\n", path);

        HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
                                   NULL, OPEN_EXISTING, 0, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            LOG("[DFBHD-WS] minimap_patch.tga 없음 → 패치 레이어 비활성\n");
            return;
        }
        DWORD fileSize = GetFileSize(hFile, NULL);
        BYTE* fileBuf  = new BYTE[fileSize];
        DWORD read = 0;
        ReadFile(hFile, fileBuf, fileSize, &read, NULL);
        CloseHandle(hFile);

        stbi_set_flip_vertically_on_load(1);

        int w, h, ch;
        unsigned char* pixels = stbi_load_from_memory(fileBuf, (int)fileSize,
                                                       &w, &h, &ch, 4);
        delete[] fileBuf;
        if (!pixels) {
            LOG("[DFBHD-WS] 미니맵 패치 TGA 디코딩 실패\n");
            return;
        }
        LOG("[DFBHD-WS] 미니맵 패치 TGA 디코딩 성공: %dx%d 원본채널수=%d\n", w, h, ch);

        HRESULT hr = m_real->CreateTexture(w, h, 1, 0,
                                           D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                           &m_mmPatchTex);
        if (FAILED(hr)) {
            LOG("[DFBHD-WS] 미니맵 패치 텍스처 생성 실패\n");
            stbi_image_free(pixels);
            return;
        }

        D3DLOCKED_RECT lr;
        m_mmPatchTex->LockRect(0, &lr, NULL, 0);
        for (int y = 0; y < h; ++y) {
            DWORD*         dst = reinterpret_cast<DWORD*>(
                                     static_cast<BYTE*>(lr.pBits) + y * lr.Pitch);
            unsigned char* src = pixels + y * w * 4;
            for (int x = 0; x < w; ++x, src += 4)
                dst[x] = (DWORD(src[3]) << 24)
                        | (DWORD(src[0]) << 16)
                        | (DWORD(src[1]) <<  8)
                        |  DWORD(src[2]);
        }
        m_mmPatchTex->UnlockRect(0);
        stbi_image_free(pixels);
        LOG("[DFBHD-WS] minimap_patch.tga 로드 완료: %dx%d\n", w, h);
    }

    // ── 미니맵 패치 전체화면 렌더링 (scope_overlay 바로 위에 한 번 더) ──────
    void DrawMinimapPatchOverlay()
    {
        if (!m_mmPatchTex) return;

        DWORD sBlend, sSrc, sDst, sZE, sZW, sFog, sAlpha;
        IDirect3DBaseTexture8* pOldTex = NULL;
        DWORD oldTSSColor, oldTSSAlpha, oldTSSCArg1, oldTSSAArg1;
        m_real->GetTexture(0, &pOldTex);
        m_real->GetRenderState(D3DRS_ALPHABLENDENABLE, &sBlend);
        m_real->GetRenderState(D3DRS_SRCBLEND,         &sSrc);
        m_real->GetRenderState(D3DRS_DESTBLEND,        &sDst);
        m_real->GetRenderState(D3DRS_ZENABLE,          &sZE);
        m_real->GetRenderState(D3DRS_ZWRITEENABLE,     &sZW);
        m_real->GetRenderState(D3DRS_FOGENABLE,        &sFog);
        m_real->GetRenderState(D3DRS_ALPHATESTENABLE,  &sAlpha);
        m_real->GetTextureStageState(0, D3DTSS_COLOROP,   &oldTSSColor);
        m_real->GetTextureStageState(0, D3DTSS_ALPHAOP,   &oldTSSAlpha);
        m_real->GetTextureStageState(0, D3DTSS_COLORARG1, &oldTSSCArg1);
        m_real->GetTextureStageState(0, D3DTSS_ALPHAARG1, &oldTSSAArg1);

        m_real->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        m_real->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
        m_real->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
        m_real->SetRenderState(D3DRS_ZENABLE,          FALSE);
        m_real->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
        m_real->SetRenderState(D3DRS_FOGENABLE,        FALSE);
        m_real->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);
        m_real->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
        m_real->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        m_real->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
        m_real->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        m_real->SetTexture(0, m_mmPatchTex);
        m_real->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);

        struct TV { float x,y,z,rhw,u,v; };
        const float W = g_screenW, H = g_screenH;
        TV v[4] = {
            { -0.5f,     -0.5f,     0.f, 1.f, 0.f, 0.f },
            { W - 0.5f,  -0.5f,     0.f, 1.f, 1.f, 0.f },
            { -0.5f,     H - 0.5f,  0.f, 1.f, 0.f, 1.f },
            { W - 0.5f,  H - 0.5f,  0.f, 1.f, 1.f, 1.f },
        };
        m_real->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(TV));

        m_real->SetTexture(0, pOldTex);
        if (pOldTex) pOldTex->Release();
        m_real->SetRenderState(D3DRS_ALPHABLENDENABLE, sBlend);
        m_real->SetRenderState(D3DRS_SRCBLEND,         sSrc);
        m_real->SetRenderState(D3DRS_DESTBLEND,        sDst);
        m_real->SetRenderState(D3DRS_ZENABLE,          sZE);
        m_real->SetRenderState(D3DRS_ZWRITEENABLE,     sZW);
        m_real->SetRenderState(D3DRS_FOGENABLE,        sFog);
        m_real->SetRenderState(D3DRS_ALPHATESTENABLE,  sAlpha);
        m_real->SetTextureStageState(0, D3DTSS_COLOROP,   oldTSSColor);
        m_real->SetTextureStageState(0, D3DTSS_ALPHAOP,   oldTSSAlpha);
        m_real->SetTextureStageState(0, D3DTSS_COLORARG1, oldTSSCArg1);
        m_real->SetTextureStageState(0, D3DTSS_ALPHAARG1, oldTSSAArg1);
        m_real->SetVertexShader(m_fvf);
    }

    // ── PNG 오버레이 전체화면 렌더링 ─────────────────────────────────────────
    void DrawScopeOverlay()
    {
        if (!m_scopeOverlayTex) return;

        // 렌더 스테이트 저장
        DWORD sBlend, sSrc, sDst, sZE, sZW, sFog, sAlpha;
        IDirect3DBaseTexture8* pOldTex = NULL;
        DWORD oldTSSColor, oldTSSAlpha, oldTSSCArg1, oldTSSAArg1;
        m_real->GetTexture(0, &pOldTex);
        m_real->GetRenderState(D3DRS_ALPHABLENDENABLE, &sBlend);
        m_real->GetRenderState(D3DRS_SRCBLEND,         &sSrc);
        m_real->GetRenderState(D3DRS_DESTBLEND,        &sDst);
        m_real->GetRenderState(D3DRS_ZENABLE,          &sZE);
        m_real->GetRenderState(D3DRS_ZWRITEENABLE,     &sZW);
        m_real->GetRenderState(D3DRS_FOGENABLE,        &sFog);
        m_real->GetRenderState(D3DRS_ALPHATESTENABLE,  &sAlpha);
        m_real->GetTextureStageState(0, D3DTSS_COLOROP,   &oldTSSColor);
        m_real->GetTextureStageState(0, D3DTSS_ALPHAOP,   &oldTSSAlpha);
        m_real->GetTextureStageState(0, D3DTSS_COLORARG1, &oldTSSCArg1);
        m_real->GetTextureStageState(0, D3DTSS_ALPHAARG1, &oldTSSAArg1);

        // 오버레이용 스테이트 설정
        m_real->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        m_real->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
        m_real->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
        m_real->SetRenderState(D3DRS_ZENABLE,          FALSE);
        m_real->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
        m_real->SetRenderState(D3DRS_FOGENABLE,        FALSE);
        m_real->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);
        m_real->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
        m_real->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        m_real->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
        m_real->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        m_real->SetTexture(0, m_scopeOverlayTex);
        m_real->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_TEX1);

        // D3D8 픽셀-텍셀 오프셋 보정 (-0.5)
        struct TV { float x,y,z,rhw,u,v; };
        const float W = g_screenW, H = g_screenH;
        TV v[4] = {
            { -0.5f,     -0.5f,     0.f, 1.f, 0.f, 0.f },
            { W - 0.5f,  -0.5f,     0.f, 1.f, 1.f, 0.f },
            { -0.5f,     H - 0.5f,  0.f, 1.f, 0.f, 1.f },
            { W - 0.5f,  H - 0.5f,  0.f, 1.f, 1.f, 1.f },
        };
        m_real->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(TV));

        // 렌더 스테이트 복원
        m_real->SetTexture(0, pOldTex);
        if (pOldTex) pOldTex->Release();
        m_real->SetRenderState(D3DRS_ALPHABLENDENABLE, sBlend);
        m_real->SetRenderState(D3DRS_SRCBLEND,         sSrc);
        m_real->SetRenderState(D3DRS_DESTBLEND,        sDst);
        m_real->SetRenderState(D3DRS_ZENABLE,          sZE);
        m_real->SetRenderState(D3DRS_ZWRITEENABLE,     sZW);
        m_real->SetRenderState(D3DRS_FOGENABLE,        sFog);
        m_real->SetRenderState(D3DRS_ALPHATESTENABLE,  sAlpha);
        m_real->SetTextureStageState(0, D3DTSS_COLOROP,   oldTSSColor);
        m_real->SetTextureStageState(0, D3DTSS_ALPHAOP,   oldTSSAlpha);
        m_real->SetTextureStageState(0, D3DTSS_COLORARG1, oldTSSCArg1);
        m_real->SetTextureStageState(0, D3DTSS_ALPHAARG1, oldTSSAArg1);
        m_real->SetVertexShader(m_fvf);
    }

    // ── 보조: 조준경 X보정 후 노출된 좌우 코너를 검은색으로 채움 ─────────────
    void DrawBlackCorners()
    {
        if (g_correction >= 0.999f) return;

        float cx        = g_screenW * 0.5f;
        float corrLeft  = cx * (1.0f - g_correction);   // 보정된 좌측 끝
        float corrRight = cx * (1.0f + g_correction);   // 보정된 우측 끝
        if (corrLeft <= 0.0f && corrRight >= g_screenW) return;

        // 최소한의 렌더 스테이트만 저장/복원
        DWORD oldBlend, oldAlpha, oldZW, oldZE;
        m_real->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldBlend);
        m_real->GetRenderState(D3DRS_ALPHATESTENABLE,  &oldAlpha);
        m_real->GetRenderState(D3DRS_ZWRITEENABLE,     &oldZW);
        m_real->GetRenderState(D3DRS_ZENABLE,          &oldZE);
        IDirect3DBaseTexture8* pTex = NULL;
        m_real->GetTexture(0, &pTex);

        m_real->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        m_real->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);
        m_real->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
        m_real->SetRenderState(D3DRS_ZENABLE,          FALSE);
        m_real->SetTexture(0, NULL);
        m_real->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

        struct BV { float x, y, z, rhw; DWORD col; };
        const DWORD K = 0xFF000000u;
        float T = 0.0f, B = g_screenH;

        if (corrLeft > 0.0f) {
            BV v[4] = {
                {0.0f,     T, 0.0f, 1.0f, K},
                {0.0f,     B, 0.0f, 1.0f, K},
                {corrLeft, T, 0.0f, 1.0f, K},
                {corrLeft, B, 0.0f, 1.0f, K},
            };
            m_real->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(BV));
        }
        if (corrRight < g_screenW) {
            BV v[4] = {
                {corrRight,  T, 0.0f, 1.0f, K},
                {corrRight,  B, 0.0f, 1.0f, K},
                {g_screenW,  T, 0.0f, 1.0f, K},
                {g_screenW,  B, 0.0f, 1.0f, K},
            };
            m_real->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(BV));
        }

        // 렌더 스테이트 복원
        m_real->SetTexture(0, pTex);
        if (pTex) pTex->Release();
        m_real->SetRenderState(D3DRS_ALPHABLENDENABLE, oldBlend);
        m_real->SetRenderState(D3DRS_ALPHATESTENABLE,  oldAlpha);
        m_real->SetRenderState(D3DRS_ZWRITEENABLE,     oldZW);
        m_real->SetRenderState(D3DRS_ZENABLE,          oldZE);
        m_real->SetVertexShader(m_fvf);   // 게임 FVF 복원 (m_fvf는 변경 안 됨)
    }

    // ── 조준선 클리핑 공통 헬퍼 ─────────────────────────────────────────────
    // pBuf: 복사된 버텍스 버퍼(수정 가능), vcount: 버텍스 수, stride: 버텍스 크기
    // 수평/수직 선을 원 안으로 클리핑. 클리핑 여부 반환.
    bool ClipCrosshairVerts(BYTE* pBuf, UINT vcount, UINT stride,
                            float cx, float cy, float r)
    {
        float minX=1e9f,maxX=-1e9f,minY=1e9f,maxY=-1e9f;
        for (UINT i = 0; i < vcount; ++i) {
            const float* v = reinterpret_cast<const float*>(pBuf + i*stride);
            if (v[0]<minX) minX=v[0]; if (v[0]>maxX) maxX=v[0];
            if (v[1]<minY) minY=v[1]; if (v[1]>maxY) maxY=v[1];
        }
        float xs = maxX-minX, ys = maxY-minY;
        bool isH = (xs > r*0.4f && ys < 40.0f);   // 수평선: 넓고 얇음
        bool isV = (ys > r*0.4f && xs < 40.0f);   // 수직선: 높고 가늘음
        bool out  = (minX < cx-r || maxX > cx+r || minY < cy-r || maxY > cy+r);

        if (!(isH || isV) || !out) return false;

        if (isH) {
            float yc = (minY+maxY)*0.5f, dy = yc-cy;
            if (fabsf(dy) < r) {
                float dx = sqrtf(r*r - dy*dy);
                for (UINT i = 0; i < vcount; ++i) {
                    float* v = reinterpret_cast<float*>(pBuf + i*stride);
                    if (v[0] < cx-dx) v[0] = cx-dx;
                    if (v[0] > cx+dx) v[0] = cx+dx;
                }
            }
        } else {
            float xc = (minX+maxX)*0.5f, dxc = xc-cx;
            if (fabsf(dxc) < r) {
                float dyc = sqrtf(r*r - dxc*dxc);
                for (UINT i = 0; i < vcount; ++i) {
                    float* v = reinterpret_cast<float*>(pBuf + i*stride);
                    if (v[1] < cy-dyc) v[1] = cy-dyc;
                    if (v[1] > cy+dyc) v[1] = cy+dyc;
                }
            }
        }
        return true;
    }

    // DrawPrimitiveUP: 조준경 쿼드는 이 경로로 렌더링됨
    HRESULT WINAPI DrawPrimitiveUP(
        D3DPRIMITIVETYPE pt, UINT pc,
        const void* pVB, UINT stride) override
    {
        // ── ① 전체화면 조준경 쿼드 감지 ─────────────────────────────────────
        if (IsFullscreenScopeQuad(pt, pc, pVB, stride, m_fvf))
        {
            LOG("[DFBHD-WS] DrawPrimitiveUP: 전체화면 쿼드 감지 (pt=%d pc=%u fvf=0x%04X expectNext=%d)\n",
                pt, pc, m_fvf, m_expectScopeQuadNext ? 1 : 0);

            // ※ 핵심 수정: "이번 프레임 내내" 플래그(m_scopeDrawnThisFrame) 대신
            //   pc=128 직후 "단 1개"의 다음 풀스크린 쿼드만 소비하는
            //   1회성 플래그(m_expectScopeQuadNext)를 사용한다.
            //   이렇게 해야 같은 프레임에 NVG 등 다른 풀스크린 효과가
            //   이어서 그려져도 TGA 로직에 잘못 걸리지 않는다.
            if (m_expectScopeQuadNext) {
                m_expectScopeQuadNext = false;  // 이 쿼드 1개만 소비 → 즉시 리셋

                if (m_scopeOverlayTex) {
                    // [TGA 모드] 게임 원본 조준경 텍스처 쿼드 억제
                    // (TGA는 이미 pc=128 처리 시점에 그려짐)
                    return S_OK;  // 게임 draw call 억제
                }
                else if (g_correction < 0.999f) {
                    // [폴백 모드] TGA 없음 → 기존 X보정 방식
                    UINT vcount = pc + 2;
                    LOG("[DFBHD-WS] 조준경 쿼드 보정(폴백) vcount=%u\n", vcount);
                    DrawBlackCorners();
                    BYTE* buf = new BYTE[vcount * stride];
                    memcpy(buf, pVB, vcount * stride);
                    CorrectQuadX(buf, vcount, stride);
                    HRESULT hr = m_real->DrawPrimitiveUP(pt, pc, buf, stride);
                    delete[] buf;
                    return hr;
                }
                // 보정 불필요(4:3 등): 그냥 통과 (아래 일반 경로로 흐름)
            }
            else if (g_correction < 0.999f) {
                // pc=128과 무관한 전체화면 쿼드 (NVG 등) → X 보정만 적용,
                // TGA/억제 로직과 완전히 분리되어 영향받지 않음
                UINT vcount = pc + 2;
                BYTE* buf = new BYTE[vcount * stride];
                memcpy(buf, pVB, vcount * stride);
                CorrectQuadX(buf, vcount, stride);
                HRESULT hr = m_real->DrawPrimitiveUP(pt, pc, buf, stride);
                delete[] buf;
                return hr;
            }
            // g_correction == 1.0이고 조준경 대상도 아니면 일반 경로로 통과
        }

        // ── ② 조준선 처리 (DrawPrimitiveUP 경유) ────────────────────────────
        if (m_scopeDrawnThisFrame && (m_fvf & D3DFVF_XYZRHW) && stride >= 8u)
        {
            float r  = m_circleKnown ? m_circleR  : g_screenH * 0.5f;
            float cx = m_circleKnown ? m_circleCX : g_screenW * 0.5f;
            float cy = m_circleKnown ? m_circleCY : g_screenH * 0.5f;

            UINT vcount = 0;
            switch (pt) {
                case D3DPT_TRIANGLELIST:  vcount = pc * 3;  break;
                case D3DPT_TRIANGLESTRIP:
                case D3DPT_TRIANGLEFAN:   vcount = pc + 2;  break;
                case D3DPT_LINELIST:      vcount = pc * 2;  break;
                case D3DPT_LINESTRIP:     vcount = pc + 1;  break;
                case D3DPT_POINTLIST:     vcount = pc;      break;
                default: break;
            }

            if (vcount >= 2 && vcount <= 1024) {
                if (m_scopeOverlayTex) {
                    // [PNG 모드] 조준선 억제
                    BYTE tmpBuf[8]; // bbox만 확인
                    const BYTE* pb = reinterpret_cast<const BYTE*>(pVB);
                    float minX=1e9f,maxX=-1e9f,minY=1e9f,maxY=-1e9f;
                    for (UINT i=0;i<vcount;++i){
                        const float* v=reinterpret_cast<const float*>(pb+i*stride);
                        if(v[0]<minX)minX=v[0]; if(v[0]>maxX)maxX=v[0];
                        if(v[1]<minY)minY=v[1]; if(v[1]>maxY)maxY=v[1];
                    }
                    float xs=maxX-minX, ys=maxY-minY;
                    bool isScopeElem = (xs > r*0.4f || ys > r*0.4f);
                    if (isScopeElem) return S_OK; // 억제
                } else {
                    // [폴백 모드] 클리핑
                    BYTE* buf = new BYTE[vcount * stride];
                    memcpy(buf, pVB, vcount * stride);
                    if (ClipCrosshairVerts(buf, vcount, stride, cx, cy, r)) {
                        HRESULT hr = m_real->DrawPrimitiveUP(pt, pc, buf, stride);
                        delete[] buf;
                        return hr;
                    }
                    delete[] buf;
                }
            }
        }

        return m_real->DrawPrimitiveUP(pt, pc, pVB, stride);
    }

    // Reset: 해상도 변경 시 보정값 갱신
    HRESULT WINAPI Reset(D3DPRESENT_PARAMETERS* pp) override {
        if (pp && pp->BackBufferWidth && pp->BackBufferHeight) {
            g_screenW    = (float)pp->BackBufferWidth;
            g_screenH    = (float)pp->BackBufferHeight;
            float aspect = g_screenW / g_screenH;
            g_correction = (4.0f / 3.0f) / aspect;
            if (g_correction > 0.999f) g_correction = 1.0f;
            m_circleKnown         = false;
            m_scopeDrawnThisFrame = false;
            m_scopeOverlayDrawn   = false;
            m_mmPatchDrawn        = false;
            // 텍스처는 MANAGED pool이므로 Reset 후 재로드 필요
            if (m_scopeOverlayTex) { m_scopeOverlayTex->Release(); m_scopeOverlayTex = NULL; }
            m_scopeTexLoaded = false;
            if (m_mmPatchTex) { m_mmPatchTex->Release(); m_mmPatchTex = NULL; }
            m_mmPatchTexLoaded = false;
            LOG("[DFBHD-WS] Reset: %ux%u correction=%.4f\n",
                pp->BackBufferWidth, pp->BackBufferHeight, g_correction);
        }
        return m_real->Reset(pp);
    }

    // ── 나머지 포워딩 ────────────────────────────────────────────────────────
    HRESULT WINAPI TestCooperativeLevel() override { return m_real->TestCooperativeLevel(); }
    UINT    WINAPI GetAvailableTextureMem() override { return m_real->GetAvailableTextureMem(); }
    HRESULT WINAPI ResourceManagerDiscardBytes(DWORD b) override { return m_real->ResourceManagerDiscardBytes(b); }
    HRESULT WINAPI GetDirect3D(IDirect3D8** p) override { return m_real->GetDirect3D(p); }
    HRESULT WINAPI GetDeviceCaps(D3DCAPS8* p) override { return m_real->GetDeviceCaps(p); }
    HRESULT WINAPI GetDisplayMode(D3DDISPLAYMODE* p) override { return m_real->GetDisplayMode(p); }
    HRESULT WINAPI GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p) override { return m_real->GetCreationParameters(p); }
    HRESULT WINAPI SetCursorProperties(UINT x, UINT y, IDirect3DSurface8* s) override { return m_real->SetCursorProperties(x,y,s); }
    void    WINAPI SetCursorPosition(UINT x, UINT y, DWORD f) override { m_real->SetCursorPosition(x,y,f); }
    BOOL    WINAPI ShowCursor(BOOL b) override { return m_real->ShowCursor(b); }
    HRESULT WINAPI CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* p, IDirect3DSwapChain8** s) override { return m_real->CreateAdditionalSwapChain(p,s); }
    HRESULT WINAPI GetBackBuffer(UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface8** s) override { return m_real->GetBackBuffer(i,t,s); }
    HRESULT WINAPI GetRasterStatus(D3DRASTER_STATUS* s) override { return m_real->GetRasterStatus(s); }
    void    WINAPI SetGammaRamp(DWORD f, const D3DGAMMARAMP* r) override { m_real->SetGammaRamp(f,r); }
    void    WINAPI GetGammaRamp(D3DGAMMARAMP* r) override { m_real->GetGammaRamp(r); }
    HRESULT WINAPI CreateTexture(UINT w,UINT h,UINT l,DWORD u,D3DFORMAT f,D3DPOOL p,IDirect3DTexture8** t) override { return m_real->CreateTexture(w,h,l,u,f,p,t); }
    HRESULT WINAPI CreateVolumeTexture(UINT w,UINT h,UINT d,UINT l,DWORD u,D3DFORMAT f,D3DPOOL p,IDirect3DVolumeTexture8** t) override { return m_real->CreateVolumeTexture(w,h,d,l,u,f,p,t); }
    HRESULT WINAPI CreateCubeTexture(UINT e,UINT l,DWORD u,D3DFORMAT f,D3DPOOL p,IDirect3DCubeTexture8** t) override { return m_real->CreateCubeTexture(e,l,u,f,p,t); }
    HRESULT WINAPI CreateVertexBuffer(UINT s,DWORD u,DWORD fvf,D3DPOOL p,IDirect3DVertexBuffer8** vb) override { return m_real->CreateVertexBuffer(s,u,fvf,p,vb); }
    HRESULT WINAPI CreateIndexBuffer(UINT s,DWORD u,D3DFORMAT f,D3DPOOL p,IDirect3DIndexBuffer8** ib) override { return m_real->CreateIndexBuffer(s,u,f,p,ib); }
    HRESULT WINAPI CreateRenderTarget(UINT w,UINT h,D3DFORMAT f,D3DMULTISAMPLE_TYPE m,BOOL l,IDirect3DSurface8** s) override { return m_real->CreateRenderTarget(w,h,f,m,l,s); }
    HRESULT WINAPI CreateDepthStencilSurface(UINT w,UINT h,D3DFORMAT f,D3DMULTISAMPLE_TYPE m,IDirect3DSurface8** s) override { return m_real->CreateDepthStencilSurface(w,h,f,m,s); }
    HRESULT WINAPI CreateImageSurface(UINT w,UINT h,D3DFORMAT f,IDirect3DSurface8** s) override { return m_real->CreateImageSurface(w,h,f,s); }
    HRESULT WINAPI CopyRects(IDirect3DSurface8* s,const RECT* r,UINT c,IDirect3DSurface8* d,const POINT* p) override { return m_real->CopyRects(s,r,c,d,p); }
    HRESULT WINAPI UpdateTexture(IDirect3DBaseTexture8* s,IDirect3DBaseTexture8* d) override { return m_real->UpdateTexture(s,d); }
    HRESULT WINAPI GetFrontBuffer(IDirect3DSurface8* s) override { return m_real->GetFrontBuffer(s); }
    HRESULT WINAPI SetRenderTarget(IDirect3DSurface8* rt,IDirect3DSurface8* ds) override { return m_real->SetRenderTarget(rt,ds); }
    HRESULT WINAPI GetRenderTarget(IDirect3DSurface8** s) override { return m_real->GetRenderTarget(s); }
    HRESULT WINAPI GetDepthStencilSurface(IDirect3DSurface8** s) override { return m_real->GetDepthStencilSurface(s); }
    HRESULT WINAPI BeginScene() override { return m_real->BeginScene(); }
    HRESULT WINAPI EndScene()   override { return m_real->EndScene(); }
    // Present: 프레임 경계 → 조준경 플래그 리셋
    HRESULT WINAPI Present(const RECT* s, const RECT* d,
                           HWND w, const RGNDATA* r) override {
        // 미니맵 패치를 여기서 그린다: 이 시점엔 미니맵을 포함한 모든
        // HUD가 이미 다 그려진 뒤이므로, 미니맵이 자신을 나중에 다시
        // 그려서 패치를 덮어버리는 문제가 없다.
        // m_scopeDrawnThisFrame은 실제 조준경 사용 중(텍스처 안 묶인
        // pc=128 폴리곤 감지)일 때만 true가 되도록 이미 검증되었으므로,
        // 메뉴 화면이나 비조준 상태에는 절대 영향을 주지 않는다.
        // 폴백: DrawPrimitive에서 미니맵 쿼드를 못 잡아낸 경우에만 여기서
        // 마지막 수단으로 그린다(이 경우 블룸 영향은 못 받지만, 최소한
        // 회색 선은 가려짐을 보장).
        if (m_scopeDrawnThisFrame && m_mmPatchTex && !m_mmPatchDrawn) {
            m_real->BeginScene();
            DrawMinimapPatchOverlay();
            m_real->EndScene();
        }

        m_scopeDrawnThisFrame  = false;
        m_scopeOverlayDrawn    = false;
        m_mmPatchDrawn         = false;
        return m_real->Present(s, d, w, r);
    }
    HRESULT WINAPI Clear(DWORD c,const D3DRECT* r,DWORD f,D3DCOLOR col,float z,DWORD s) override { return m_real->Clear(c,r,f,col,z,s); }
    HRESULT WINAPI SetTransform(D3DTRANSFORMSTATETYPE t,const D3DMATRIX* m) override { return m_real->SetTransform(t,m); }
    HRESULT WINAPI GetTransform(D3DTRANSFORMSTATETYPE t,D3DMATRIX* m) override { return m_real->GetTransform(t,m); }
    HRESULT WINAPI MultiplyTransform(D3DTRANSFORMSTATETYPE t,const D3DMATRIX* m) override { return m_real->MultiplyTransform(t,m); }
    HRESULT WINAPI SetViewport(const D3DVIEWPORT8* v) override {
        // 게임이 설정하는 뷰포트를 기억해 두고, DrawPrimitive 보정 후 이 값으로 복원
        if (v) { m_gameVP = *v; m_gameVPSet = true; }
        return m_real->SetViewport(v);
    }
    HRESULT WINAPI GetViewport(D3DVIEWPORT8* v) override { return m_real->GetViewport(v); }
    HRESULT WINAPI SetMaterial(const D3DMATERIAL8* m) override { return m_real->SetMaterial(m); }
    HRESULT WINAPI GetMaterial(D3DMATERIAL8* m) override { return m_real->GetMaterial(m); }
    HRESULT WINAPI SetLight(DWORD i,const D3DLIGHT8* l) override { return m_real->SetLight(i,l); }
    HRESULT WINAPI GetLight(DWORD i,D3DLIGHT8* l) override { return m_real->GetLight(i,l); }
    HRESULT WINAPI LightEnable(DWORD i,BOOL e) override { return m_real->LightEnable(i,e); }
    HRESULT WINAPI GetLightEnable(DWORD i,BOOL* e) override { return m_real->GetLightEnable(i,e); }
    HRESULT WINAPI SetClipPlane(DWORD i,const float* p) override { return m_real->SetClipPlane(i,p); }
    HRESULT WINAPI GetClipPlane(DWORD i,float* p) override { return m_real->GetClipPlane(i,p); }
    HRESULT WINAPI SetRenderState(D3DRENDERSTATETYPE s,DWORD v) override { return m_real->SetRenderState(s,v); }
    HRESULT WINAPI GetRenderState(D3DRENDERSTATETYPE s,DWORD* v) override { return m_real->GetRenderState(s,v); }
    HRESULT WINAPI BeginStateBlock() override { return m_real->BeginStateBlock(); }
    HRESULT WINAPI EndStateBlock(DWORD* t) override { return m_real->EndStateBlock(t); }
    HRESULT WINAPI ApplyStateBlock(DWORD t) override { return m_real->ApplyStateBlock(t); }
    HRESULT WINAPI CaptureStateBlock(DWORD t) override { return m_real->CaptureStateBlock(t); }
    HRESULT WINAPI DeleteStateBlock(DWORD t) override { return m_real->DeleteStateBlock(t); }
    HRESULT WINAPI CreateStateBlock(D3DSTATEBLOCKTYPE t,DWORD* h) override { return m_real->CreateStateBlock(t,h); }
    HRESULT WINAPI SetClipStatus(const D3DCLIPSTATUS8* c) override { return m_real->SetClipStatus(c); }
    HRESULT WINAPI GetClipStatus(D3DCLIPSTATUS8* c) override { return m_real->GetClipStatus(c); }
    HRESULT WINAPI GetTexture(DWORD s,IDirect3DBaseTexture8** t) override { return m_real->GetTexture(s,t); }
    HRESULT WINAPI SetTexture(DWORD s,IDirect3DBaseTexture8* t) override { return m_real->SetTexture(s,t); }
    HRESULT WINAPI GetTextureStageState(DWORD s,D3DTEXTURESTAGESTATETYPE t,DWORD* v) override { return m_real->GetTextureStageState(s,t,v); }
    HRESULT WINAPI SetTextureStageState(DWORD s,D3DTEXTURESTAGESTATETYPE t,DWORD v) override { return m_real->SetTextureStageState(s,t,v); }
    HRESULT WINAPI ValidateDevice(DWORD* p) override { return m_real->ValidateDevice(p); }
    HRESULT WINAPI GetInfo(DWORD id,void* p,DWORD s) override { return m_real->GetInfo(id,p,s); }
    HRESULT WINAPI SetPaletteEntries(UINT n,const PALETTEENTRY* e) override { return m_real->SetPaletteEntries(n,e); }
    HRESULT WINAPI GetPaletteEntries(UINT n,PALETTEENTRY* e) override { return m_real->GetPaletteEntries(n,e); }
    HRESULT WINAPI SetCurrentTexturePalette(UINT n) override { return m_real->SetCurrentTexturePalette(n); }
    HRESULT WINAPI GetCurrentTexturePalette(UINT* n) override { return m_real->GetCurrentTexturePalette(n); }
    // DrawPrimitive: 조준경 원 → DYNAMIC VB 교체 방식으로 X좌표 보정
    //
    // XYZRHW 버텍스는 SetViewport 스케일링을 받지 않으므로,
    // 게임 VB를 읽어서 X좌표를 보정한 DYNAMIC VB로 교체 후 Draw한다.
    //
    // 조준경 원 시그니처: TRISTRIP + pc=128 + start=0 + FVF=0x02C4
    HRESULT WINAPI DrawPrimitive(D3DPRIMITIVETYPE t, UINT startV, UINT pc) override
    {
        // ── 미니맵 자체 재그리기 감지 → 그 직후 패치를 덧그림 ────────────────
        // 패치를 Present 시점까지 미루면 미니맵은 잘 덮지만, 그보다도 더
        // 늦게 적용되는 블룸(태양 노출) 포스트프로세스의 영향을 전혀 받지
        // 못해 햇빛 볼 때 패치만 도드라지는 문제가 있었다.
        // → 미니맵의 "자기 자신 다시 그리기" 호출 직후에 패치를 끼워넣으면
        //   미니맵은 가리면서도 그 뒤에 오는 블룸은 우리 패치에도 똑같이
        //   적용되어 자연스럽게 묻어간다.
        if (m_scopeDrawnThisFrame && m_mmPatchTex && !m_mmPatchDrawn
            && pc == 2u
            && (t == D3DPT_TRIANGLESTRIP || t == D3DPT_TRIANGLEFAN)
            && (m_fvf & D3DFVF_XYZRHW)
            && m_capturedVB != NULL
            && m_capturedStride >= 8u)
        {
            IDirect3DBaseTexture8* pMmTex = NULL;
            m_real->GetTexture(0, &pMmTex);
            bool mmTexBound = (pMmTex != NULL);
            if (pMmTex) pMmTex->Release();

            if (mmTexBound) {
                BYTE* pSrcMM = NULL;
                UINT offMM = startV * m_capturedStride;
                UINT lenMM = 4u * m_capturedStride;
                if (SUCCEEDED(m_capturedVB->Lock(offMM, lenMM, &pSrcMM, D3DLOCK_READONLY)) && pSrcMM) {
                    float mnX=1e9f,mxX=-1e9f,mnY=1e9f,mxY=-1e9f;
                    for (UINT i=0;i<4;++i){
                        const float* v=reinterpret_cast<const float*>(pSrcMM+i*m_capturedStride);
                        if(v[0]<mnX)mnX=v[0]; if(v[0]>mxX)mxX=v[0];
                        if(v[1]<mnY)mnY=v[1]; if(v[1]>mxY)mxY=v[1];
                    }
                    m_capturedVB->Unlock();
                    float qcx=(mnX+mxX)*0.5f, qcy=(mnY+mxY)*0.5f;
                    bool inMinimapRegion = (qcx >= g_screenW*0.78f) && (qcy >= g_screenH*0.65f);

                    if (inMinimapRegion) {
                        // 원본 미니맵 draw call은 그대로 통과시키고,
                        HRESULT hrMM = m_real->DrawPrimitive(t, startV, pc);
                        // 그 직후 패치를 덧그려 가린다.
                        DrawMinimapPatchOverlay();
                        m_mmPatchDrawn = true;
                        return hrMM;
                    }
                }
            }
        }

        if (g_correction < 0.999f
            && t      == D3DPT_TRIANGLESTRIP
            && pc     == 128u
            && startV == 0u
            && m_fvf  == 0x02C4u
            && m_capturedVB != NULL
            && m_capturedStride >= 8u)
        {
            // ── 핵심: 보정 적용 전에 텍스처 바인딩 여부부터 확인 ───────────────
            // (로그로 검증됨: 텍스처 있음 = NVG 단독/스코프 동시 사용 중)
            // NVG 결합 모드에서는 우리가 하늘 폴리곤만 X보정하면
            // 게임 자신의 (보정 안 된) 테두리/마스크와 어긋나
            // "O자형 초승달" 불일치 무늬가 발생한다.
            // → NVG 결합 모드에서는 보정을 아예 적용하지 않고 원본 그대로 통과.
            IDirect3DBaseTexture8* pPreTex = NULL;
            m_real->GetTexture(0, &pPreTex);
            bool texBoundPre = (pPreTex != NULL);
            if (pPreTex) pPreTex->Release();

            if (texBoundPre) {
                // NVG(단독 또는 스코프 동시) → 무보정 원본 패스스루
                goto fallback;
            }

            const UINT  vcount   = pc + 2;          // 130개 버텍스
            const UINT  dataSize = vcount * m_capturedStride;
            const float cx       = g_screenW * 0.5f;

            // ── 보정용 DYNAMIC VB 생성 (최초 1회 또는 크기 변경 시) ─────────
            if (!m_scopeVB || m_scopeVBSize < dataSize) {
                if (m_scopeVB) { m_scopeVB->Release(); m_scopeVB = NULL; }
                HRESULT hrc = m_real->CreateVertexBuffer(
                    dataSize,
                    D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                    m_fvf,
                    D3DPOOL_DEFAULT,
                    &m_scopeVB);
                if (FAILED(hrc) || !m_scopeVB) {
                    LOG("[DFBHD-WS] DYNAMIC VB 생성 실패\n");
                    goto fallback;
                }
                m_scopeVBSize  = dataSize;
                m_scopeVBDirty = true;
            }

            // ── 게임 VB에서 읽기 ──────────────────────────────────────────────
            // 매 프레임 읽어야 하는 이유: 배율 변경 시 좌표가 달라질 수 있음
            {
                BYTE* pSrc = NULL;
                // READONLY 플래그로 읽기 시도
                HRESULT hrLock = m_capturedVB->Lock(0, dataSize, &pSrc, D3DLOCK_READONLY);
                if (FAILED(hrLock) || !pSrc) {
                    // READONLY 실패 → NOOVERWRITE 시도
                    hrLock = m_capturedVB->Lock(0, dataSize, &pSrc, D3DLOCK_NOOVERWRITE);
                }
                if (FAILED(hrLock) || !pSrc) {
                    LOG("[DFBHD-WS] 게임 VB Lock 실패 (hr=0x%08lX)\n", (unsigned long)hrLock);
                    goto fallback;
                }

                // ── 원 파라미터 추출 (Y 좌표 기준, 보정 전 원본 데이터 사용) ──────
                {
                    float minYv = 1e9f, maxYv = -1e9f;
                    for (UINT i = 0; i < vcount; ++i) {
                        const float* v = reinterpret_cast<const float*>(
                            pSrc + i * m_capturedStride);
                        if (v[1] < minYv) minYv = v[1];
                        if (v[1] > maxYv) maxYv = v[1];
                    }
                    m_circleCX    = g_screenW * 0.5f;
                    m_circleCY    = (minYv + maxYv) * 0.5f;
                    m_circleR     = (maxYv - minYv) * 0.5f;
                    m_circleKnown = true;
                    LOG("[DFBHD-WS] 원 파라미터: cx=%.1f cy=%.1f r=%.1f\n",
                        m_circleCX, m_circleCY, m_circleR);
                }

                // ── DYNAMIC VB에 X보정해서 쓰기 ─────────────────────────────
                BYTE* pDst = NULL;
                HRESULT hrDst = m_scopeVB->Lock(0, dataSize, &pDst, D3DLOCK_DISCARD);
                if (FAILED(hrDst) || !pDst) {
                    m_capturedVB->Unlock();
                    LOG("[DFBHD-WS] DYNAMIC VB Lock 실패\n");
                    goto fallback;
                }

                memcpy(pDst, pSrc, dataSize);
                m_capturedVB->Unlock();

                // X좌표 보정 (XYZRHW: float[0] = x)
                for (UINT i = 0; i < vcount; ++i) {
                    float* x = reinterpret_cast<float*>(pDst + i * m_capturedStride);
                    *x = cx + (*x - cx) * g_correction;
                }
                m_scopeVB->Unlock();
            }

            // ── 보정 VB로 교체하여 Draw ───────────────────────────────────────
            m_real->SetStreamSource(0, m_scopeVB, m_capturedStride);
            HRESULT hr = m_real->DrawPrimitive(t, 0, pc);
            m_real->SetStreamSource(0, m_capturedVB, m_capturedStride);

            // ── 여기 도달했다면 texBound==false(순수 스코프)가 보장됨 ───────────
            // → TGA 렌더링 + DIP 억제 활성화
            m_scopeDrawnThisFrame = true;
            m_expectScopeQuadNext = true;
            EnsureScopeTexture();
            if (m_scopeOverlayTex && !m_scopeOverlayDrawn) {
                DrawScopeOverlay();
                m_scopeOverlayDrawn = true;
            }
            // 패치 텍스처는 여기서 로드만 해두고, 실제 그리기는 Present
            // 직전(미니맵 등 HUD가 전부 그려진 뒤)으로 미룬다 — 그래야 미니맵이
            // 나중에 자신을 다시 그려서 패치를 덮어버리는 문제가 없다.
            EnsureMinimapPatchTexture();
            if (m_scopeOverlayTex) return S_OK;
            return hr;
        }

        // ── 조준선 처리 (DrawPrimitive 경유 / VB 읽기) ──────────────────────
        if (m_scopeDrawnThisFrame
            && (m_fvf & D3DFVF_XYZRHW)
            && startV == 0
            && m_capturedVB && m_capturedStride >= 8u)
        {
            float r2  = m_circleKnown ? m_circleR  : g_screenH * 0.5f;
            float cx2 = m_circleKnown ? m_circleCX : g_screenW * 0.5f;
            float cy2 = m_circleKnown ? m_circleCY : g_screenH * 0.5f;

            UINT vcount2 = 0;
            switch (t) {
                case D3DPT_TRIANGLELIST:  vcount2 = pc * 3;   break;
                case D3DPT_TRIANGLESTRIP:
                case D3DPT_TRIANGLEFAN:   vcount2 = pc + 2;   break;
                case D3DPT_LINELIST:      vcount2 = pc * 2;   break;
                case D3DPT_LINESTRIP:     vcount2 = pc + 1;   break;
                default: break;
            }
            if (vcount2 >= 2 && vcount2 <= 1024) {
                UINT dataSize2 = vcount2 * m_capturedStride;
                BYTE* pSrc2 = NULL;
                HRESULT hrL = m_capturedVB->Lock(0, dataSize2, &pSrc2, D3DLOCK_READONLY);
                if (FAILED(hrL) || !pSrc2)
                    hrL = m_capturedVB->Lock(0, dataSize2, &pSrc2, D3DLOCK_NOOVERWRITE);

                if (SUCCEEDED(hrL) && pSrc2) {
                    if (m_scopeOverlayTex) {
                        // [PNG 모드] 조준선 억제
                        float minX=1e9f,maxX=-1e9f,minY=1e9f,maxY=-1e9f;
                        for (UINT i=0;i<vcount2;++i){
                            const float* v=reinterpret_cast<const float*>(
                                pSrc2+i*m_capturedStride);
                            if(v[0]<minX)minX=v[0]; if(v[0]>maxX)maxX=v[0];
                            if(v[1]<minY)minY=v[1]; if(v[1]>maxY)maxY=v[1];
                        }
                        float xs=maxX-minX, ys=maxY-minY;
                        bool isScopeElem = (xs > r2*0.4f || ys > r2*0.4f);
                        m_capturedVB->Unlock();
                        if (isScopeElem) return S_OK;
                    } else {
                        // [폴백 모드] 클리핑
                        BYTE* buf2 = new BYTE[dataSize2];
                        memcpy(buf2, pSrc2, dataSize2);
                        m_capturedVB->Unlock();
                        if (ClipCrosshairVerts(buf2, vcount2, m_capturedStride, cx2, cy2, r2)) {
                            HRESULT hr2 = m_real->DrawPrimitiveUP(t, pc, buf2, m_capturedStride);
                            delete[] buf2;
                            return hr2;
                        }
                        delete[] buf2;
                    }
                }
            }
        }

    fallback:
        return m_real->DrawPrimitive(t, startV, pc);
    }
    // DrawIndexedPrimitive: 로그로 검증 완료 — 이 경로는 게임 네이티브
    // 조준선(틱마크/십자선)이 사용한다. m_scopeDrawnThisFrame은 이제
    // pc=128 시점에 텍스처가 "안 묶여있을 때"(=순수 스코프, NVG 아님)만
    // true가 되도록 보강되었으므로, 이 조건에서만 안전하게 억제 가능.
    // (검증: 스코프만→texBound=0 100%, NVG만/NVG+스코프→texBound=1 100%)
    HRESULT WINAPI DrawIndexedPrimitive(
        D3DPRIMITIVETYPE t, UINT minIdx, UINT numVerts,
        UINT startIdx, UINT pc) override
    {
        if (m_scopeDrawnThisFrame && (m_fvf & D3DFVF_XYZRHW) && numVerts <= 512) {
            return S_OK;  // 순수 스코프 프레임의 조준선/틱마크 억제
        }
        return m_real->DrawIndexedPrimitive(t, minIdx, numVerts, startIdx, pc);
    }

    // DrawIndexedPrimitiveUP: 동일 조건으로 억제 (로그상 실사용 0건이었지만
    // 다른 무기/스코프 종류에서 쓰일 가능성 대비해 동일하게 처리)
    HRESULT WINAPI DrawIndexedPrimitiveUP(
        D3DPRIMITIVETYPE pt, UINT minIdx, UINT numVerts, UINT pc,
        const void* ib, D3DFORMAT ibFmt,
        const void* vb, UINT vStride) override
    {
        if (m_scopeDrawnThisFrame && (m_fvf & D3DFVF_XYZRHW) && numVerts <= 512) {
            return S_OK;
        }
        return m_real->DrawIndexedPrimitiveUP(pt, minIdx, numVerts, pc, ib, ibFmt, vb, vStride);
    }
    HRESULT WINAPI ProcessVertices(UINT ss,UINT ds,UINT vc,IDirect3DVertexBuffer8* dvb,DWORD f) override { return m_real->ProcessVertices(ss,ds,vc,dvb,f); }
    HRESULT WINAPI CreateVertexShader(const DWORD* d,const DWORD* t,DWORD* h,DWORD u) override { return m_real->CreateVertexShader(d,t,h,u); }
    HRESULT WINAPI GetVertexShader(DWORD* h) override { return m_real->GetVertexShader(h); }
    HRESULT WINAPI DeleteVertexShader(DWORD h) override { return m_real->DeleteVertexShader(h); }
    HRESULT WINAPI SetVertexShaderConstant(DWORD r,const void* d,DWORD c) override { return m_real->SetVertexShaderConstant(r,d,c); }
    HRESULT WINAPI GetVertexShaderConstant(DWORD r,void* d,DWORD c) override { return m_real->GetVertexShaderConstant(r,d,c); }
    HRESULT WINAPI GetVertexShaderDeclaration(DWORD h,void* d,DWORD* s) override { return m_real->GetVertexShaderDeclaration(h,d,s); }
    HRESULT WINAPI GetVertexShaderFunction(DWORD h,void* d,DWORD* s) override { return m_real->GetVertexShaderFunction(h,d,s); }
    HRESULT WINAPI SetStreamSource(UINT n, IDirect3DVertexBuffer8* vb, UINT s) override {
        if (n == 0) {
            // 스트림 0이 바뀔 때마다 추적
            if (m_capturedVB) { m_capturedVB->Release(); m_capturedVB = NULL; }
            m_capturedVB     = vb;
            m_capturedStride = s;
            if (m_capturedVB) m_capturedVB->AddRef();
        }
        return m_real->SetStreamSource(n, vb, s);
    }
    HRESULT WINAPI GetStreamSource(UINT n,IDirect3DVertexBuffer8** vb,UINT* s) override { return m_real->GetStreamSource(n,vb,s); }
    HRESULT WINAPI SetIndices(IDirect3DIndexBuffer8* ib,UINT bv) override { return m_real->SetIndices(ib,bv); }
    HRESULT WINAPI GetIndices(IDirect3DIndexBuffer8** ib,UINT* bv) override { return m_real->GetIndices(ib,bv); }
    HRESULT WINAPI CreatePixelShader(const DWORD* fn,DWORD* h) override { return m_real->CreatePixelShader(fn,h); }
    HRESULT WINAPI SetPixelShader(DWORD h) override { return m_real->SetPixelShader(h); }
    HRESULT WINAPI GetPixelShader(DWORD* h) override { return m_real->GetPixelShader(h); }
    HRESULT WINAPI DeletePixelShader(DWORD h) override { return m_real->DeletePixelShader(h); }
    HRESULT WINAPI SetPixelShaderConstant(DWORD r,const void* d,DWORD c) override { return m_real->SetPixelShaderConstant(r,d,c); }
    HRESULT WINAPI GetPixelShaderConstant(DWORD r,void* d,DWORD c) override { return m_real->GetPixelShaderConstant(r,d,c); }
    HRESULT WINAPI GetPixelShaderFunction(DWORD h,void* d,DWORD* s) override { return m_real->GetPixelShaderFunction(h,d,s); }
    HRESULT WINAPI DrawRectPatch(UINT h,const float* n,const D3DRECTPATCH_INFO* i) override { return m_real->DrawRectPatch(h,n,i); }
    HRESULT WINAPI DrawTriPatch(UINT h,const float* n,const D3DTRIPATCH_INFO* i) override { return m_real->DrawTriPatch(h,n,i); }
    HRESULT WINAPI DeletePatch(UINT h) override { return m_real->DeletePatch(h); }

private:
    virtual ~ProxyDevice8() {
        if (m_scopeVB)        { m_scopeVB->Release();        m_scopeVB        = NULL; }
        if (m_capturedVB)     { m_capturedVB->Release();     m_capturedVB     = NULL; }
        if (m_scopeOverlayTex){ m_scopeOverlayTex->Release();m_scopeOverlayTex= NULL; }
        if (m_mmPatchTex)     { m_mmPatchTex->Release();     m_mmPatchTex     = NULL; }
    }
    IDirect3DDevice8*      m_real;
    ULONG                  m_ref;
    DWORD                  m_fvf;
    D3DVIEWPORT8           m_gameVP;
    bool                   m_gameVPSet;
    // 조준경 VB 교체용
    IDirect3DVertexBuffer8* m_capturedVB;   // 게임이 바인딩한 VB
    UINT                    m_capturedStride;
    IDirect3DVertexBuffer8* m_scopeVB;      // 보정용 DYNAMIC VB
    UINT                    m_scopeVBSize;
    bool                    m_scopeVBDirty; // 재생성 필요 여부
    // 조준경 원 파라미터 (조준선 클리핑용)
    bool  m_circleKnown;        // DrawPrimitive(pc=128)에서 추출 완료 여부
    float m_circleCX;           // 원 중심 X (픽셀)
    float m_circleCY;           // 원 중심 Y (픽셀)
    float m_circleR;            // 원 반지름 (픽셀, Y 기준)
    bool  m_scopeDrawnThisFrame;// 이번 프레임에 조준경 원(pc=128)이 그려졌는지 (크로스헤어 억제용, 프레임 내내 유지)
    bool  m_expectScopeQuadNext;// pc=128 직후 다음 풀스크린 쿼드 "1개만" 스코프 오버레이로 간주 (1회성, NVG 등 이후 쿼드와 구분)
    // PNG 오버레이
    IDirect3DTexture8* m_scopeOverlayTex;  // scope_overlay.png 텍스처
    bool               m_scopeTexLoaded;   // 로드 시도 여부 (실패해도 재시도 안 함)
    bool               m_scopeOverlayDrawn;// 이번 프레임에 오버레이 이미 그림
    // 두 번째 레이어: 미니맵 회색 선 등을 가리는 패치용 TGA (조준경 시에만)
    IDirect3DTexture8* m_mmPatchTex;       // minimap_patch.tga 텍스처
    bool               m_mmPatchTexLoaded; // 로드 시도 여부
    bool               m_mmPatchDrawn;     // 이번 프레임에 이미 그림
};

// ─── IDirect3D8 프록시 ───────────────────────────────────────────────────────

class ProxyD3D8 : public IDirect3D8
{
public:
    explicit ProxyD3D8(IDirect3D8* real) : m_real(real), m_ref(1) {}

    HRESULT  WINAPI QueryInterface(REFIID r,void** p) override { return m_real->QueryInterface(r,p); }
    ULONG    WINAPI AddRef()  override { m_real->AddRef(); return ++m_ref; }
    ULONG    WINAPI Release() override {
        ULONG ref = --m_ref;
        if (ref == 0) { m_real->Release(); delete this; }
        return ref;
    }

    HRESULT  WINAPI RegisterSoftwareDevice(void* p) override { return m_real->RegisterSoftwareDevice(p); }
    UINT     WINAPI GetAdapterCount() override { return m_real->GetAdapterCount(); }
    HRESULT  WINAPI GetAdapterIdentifier(UINT a,DWORD f,D3DADAPTER_IDENTIFIER8* id) override { return m_real->GetAdapterIdentifier(a,f,id); }
    UINT     WINAPI GetAdapterModeCount(UINT a) override { return m_real->GetAdapterModeCount(a); }
    HRESULT  WINAPI EnumAdapterModes(UINT a,UINT m,D3DDISPLAYMODE* d) override { return m_real->EnumAdapterModes(a,m,d); }
    HRESULT  WINAPI GetAdapterDisplayMode(UINT a,D3DDISPLAYMODE* d) override { return m_real->GetAdapterDisplayMode(a,d); }
    HRESULT  WINAPI CheckDeviceType(UINT a,D3DDEVTYPE dt,D3DFORMAT af,D3DFORMAT bf,BOOL w) override { return m_real->CheckDeviceType(a,dt,af,bf,w); }
    HRESULT  WINAPI CheckDeviceFormat(UINT a,D3DDEVTYPE dt,D3DFORMAT af,DWORD u,D3DRESOURCETYPE rt,D3DFORMAT cf) override { return m_real->CheckDeviceFormat(a,dt,af,u,rt,cf); }
    HRESULT  WINAPI CheckDeviceMultiSampleType(UINT a,D3DDEVTYPE dt,D3DFORMAT f,BOOL w,D3DMULTISAMPLE_TYPE ms) override { return m_real->CheckDeviceMultiSampleType(a,dt,f,w,ms); }
    HRESULT  WINAPI CheckDepthStencilMatch(UINT a,D3DDEVTYPE dt,D3DFORMAT af,D3DFORMAT rf,D3DFORMAT df) override { return m_real->CheckDepthStencilMatch(a,dt,af,rf,df); }
    HRESULT  WINAPI GetDeviceCaps(UINT a,D3DDEVTYPE dt,D3DCAPS8* c) override { return m_real->GetDeviceCaps(a,dt,c); }
    HMONITOR WINAPI GetAdapterMonitor(UINT a) override { return m_real->GetAdapterMonitor(a); }

    HRESULT WINAPI CreateDevice(
        UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocus,
        DWORD Flags, D3DPRESENT_PARAMETERS* pPP,
        IDirect3DDevice8** ppDev) override
    {
        UINT w = pPP ? pPP->BackBufferWidth  : 0;
        UINT h = pPP ? pPP->BackBufferHeight : 0;
        if (!w || !h) {
            RECT rc; GetClientRect(hFocus, &rc);
            w = rc.right - rc.left;
            h = rc.bottom - rc.top;
        }
        LOG("[DFBHD-WS] CreateDevice: %ux%u\n", w, h);

        IDirect3DDevice8* real = NULL;
        HRESULT hr = m_real->CreateDevice(Adapter,DeviceType,hFocus,Flags,pPP,&real);
        if (SUCCEEDED(hr) && real) {
            // CreateDevice가 refcount=1로 반환 → ProxyDevice8이 소유권 가짐
            // real->Release() 호출 금지: refcount=0 → Device 소멸 → dangling pointer
            // → use-after-free → D3D8 내부 CRITICAL_SECTION 오염 → AV @ ntdll
            *ppDev = new ProxyDevice8(real, w, h);
        }
        return hr;
    }

private:
    virtual ~ProxyD3D8() {}
    IDirect3D8* m_real;
    ULONG       m_ref;
};

// ─── DLL 진입점 ──────────────────────────────────────────────────────────────

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hDll = hInst;
        DisableThreadLibraryCalls(hInst);
        // 로그 파일 비활성화 (디버깅 완료)
        // 진단용 빌드: 로그 활성화 (문제 해결 후 다시 주석 처리 예정)
        // g_log = fopen("dfbhd_ws.log", "w");
    }
    else if (reason == DLL_PROCESS_DETACH) {
        if (g_log)       { fclose(g_log); g_log = NULL; }
        if (g_hRealD3D8) { FreeLibrary(g_hRealD3D8); g_hRealD3D8 = NULL; }
    }
    return TRUE;
}

extern "C" __declspec(dllexport)
IDirect3D8* WINAPI Direct3DCreate8(UINT SDKVersion)
{
    // DllMain이 아닌 여기서 최초 1회 실제 DLL 로드 (Loader Lock 해제 후 시점)
    if (!InitRealD3D8()) return NULL;

    IDirect3D8* real = g_RealCreate8(SDKVersion);
    if (!real) return NULL;
    LOG("[DFBHD-WS] Direct3DCreate8 후킹 성공\n");
    return new ProxyD3D8(real);
}
