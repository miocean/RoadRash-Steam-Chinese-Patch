// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <vector>
#include <string>
#include <cstdio>
#include <intrin.h>
#include <array>
#include <unordered_map>
#include "hd_raster.hpp"
#include "hd_geometry.hpp"
#include "hd_gpu.hpp"
#include "hd_version.hpp"

namespace {
using rrhd::Vertex;
using rrhd::Texture;
uintptr_t base;
template<class T> T& at(uintptr_t va) { return *reinterpret_cast<T*>(base+va-0x400000); }
template<class T> T fn(uintptr_t va) { return reinterpret_cast<T>(base+va-0x400000); }
using DrawList = int (__cdecl*)();
using AnimationFrame = uint8_t* (__fastcall*)(uint8_t*, int, int, int*);
using Normalize = bool (__fastcall*)(int*, int);
using Actor = void (__fastcall*)(void*);
using HorizonLoad = int (__fastcall*)(const char*);
using HorizonDraw = void (__fastcall*)(int, int);
using MenuDecode = uint16_t* (__fastcall*)(const char*);
using Present8 = void (__fastcall*)(const uint8_t*, int, const uint8_t*);
using Present16 = void (__fastcall*)(const void*, int, int, int, int);
using Stretch = decltype(&StretchDIBits);
Stretch originalStretch;
using Fill = decltype(&FillRect);
Fill originalFill;
// Direct3D 11 back end (src/hd_gpu.hpp). Active only when it initializes; otherwise everything
// below falls back to the CPU/GDI path unchanged.
rrhd::GpuRenderer gpu;
bool gpuRequested=true,gpuSceneReady=false,gpuHorizonDirty=false,gpuCompare=false;
int gpuVSync=0;
std::vector<uint8_t> legacyBackdrop(640*480),legacyPicture(640*480);
std::vector<uint32_t> gpuShot;
uint32_t lastColors[256]{};
HMODULE rendererModule;
HWND hookedWindow;
WNDPROC previousWindowProc;
bool completedFrame=false;
int renderHeight=0, width=0, height=0, clientWidth=0, clientHeight=0, logicalWidth=320;
bool wideGame=true;                 // [Window] WideWindow: false = the original 4:3 picture, centred, black bars around it
int boxX=0, boxY=0, boxW=0, boxH=0; // where the picture goes in the client area
double sceneScale=1, sceneOffset=0;
bool enabled=true, frameActive=false, begun=false, worldReady=false, displayReady=false;
uint64_t frames=0, primitives=0, batches=0;
std::vector<uint8_t> world, baseline, horizon;
int horizonWidth=0, horizonRows=0, horizonScroll=0, horizonY=0;
std::vector<uint32_t> rgba, beforeText;
std::vector<int> mapX, mapY, sceneX, sceneY;
std::vector<int> backdropX, panoramaX;
int uiX[641]{},uiY[481]{};
std::vector<uint16_t> menuBright, menuDim;
std::vector<uint32_t> menuBackdrop;
bool menuBackdropDirty=true;
bool menuBackdropDim=false;
// [Graphics] SmoothText=1: text and interface are scaled with bilinear filtering instead of nearest. Menus: the 640x480 menu
// picture is sampled bilinearly into the window (cached while the picture does not change); the race HUD and text layer is
// drawn with the smoothing shader of the Direct3D renderer.
bool smoothUi=false;
std::vector<int> smoothX0,smoothX1,smoothY0,smoothY1;std::vector<unsigned> smoothWX,smoothWY;   // per output column / row
bool menuCacheValid=false;uint64_t menuCacheHash=0;
uint32_t color16[65536]{};
int colorMasks[3]{};
std::wstring configPath, logPath;

struct ResolutionChoice {int height,width;std::wstring label;};
std::vector<ResolutionChoice> resolutionChoices(int cw,int ch,int selected) {
    std::vector<ResolutionChoice> result;
    result.push_back({0,cw,L"Auto (native "+std::to_wstring(cw)+L" × "+std::to_wstring(ch)+L")"});
    std::vector<int> heights={ch,2160,1600,1440,1080,900,720,540,480};
    if(selected>0)heights.push_back(selected);
    std::sort(heights.begin(),heights.end(),std::greater<int>());
    heights.erase(std::unique(heights.begin(),heights.end()),heights.end());
    for(int h:heights)if(h<=ch || h==selected) {
        int w=std::max(1,int(std::lround(double(cw)*h/ch)));
        result.push_back({h,w,std::to_wstring(w)+L" × "+std::to_wstring(h)});
    }
    return result;
}
struct ResolutionDialog {int cw,ch;std::vector<ResolutionChoice> choices;int chosen=-1;};
INT_PTR CALLBACK resolutionDialogProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto state=reinterpret_cast<ResolutionDialog*>(GetWindowLongPtrW(window,DWLP_USER));
    if(message==WM_INITDIALOG) {
        state=reinterpret_cast<ResolutionDialog*>(lp);SetWindowLongPtrW(window,DWLP_USER,lp);
        std::wstring title=L"Current window: "+std::to_wstring(state->cw)+L" × "+std::to_wstring(state->ch);
        SetDlgItemTextW(window,1001,title.c_str());int selected=0;
        for(size_t i=0;i<state->choices.size();++i) {
            SendDlgItemMessageW(window,1002,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(state->choices[i].label.c_str()));
            if(state->choices[i].height==renderHeight)selected=int(i);
        }
        SendDlgItemMessageW(window,1002,CB_SETCURSEL,selected,0);
        RECT owner{},r{};GetWindowRect(GetParent(window),&owner);GetWindowRect(window,&r);
        SetWindowPos(window,nullptr,(owner.left+owner.right-r.right+r.left)/2,
                     (owner.top+owner.bottom-r.bottom+r.top)/2,0,0,SWP_NOSIZE|SWP_NOZORDER);
        return TRUE;
    }
    if(message==WM_COMMAND && state) {
        if(LOWORD(wp)==IDOK) {
            int selected=int(SendDlgItemMessageW(window,1002,CB_GETCURSEL,0,0));
            if(selected>=0 && selected<int(state->choices.size()))state->chosen=selected;
            EndDialog(window,IDOK);return TRUE;
        }
        if(LOWORD(wp)==IDCANCEL){EndDialog(window,IDCANCEL);return TRUE;}
    }
    if(message==WM_CLOSE){EndDialog(window,IDCANCEL);return TRUE;}
    return FALSE;
}
// While one of our own dialogs or a message box is open the controller belongs to that dialog: the game's menu
// keeps polling the pad inside the nested message loop and would act on the same button press (it opened the
// Race Resolution dialog a second time and could not be closed with the pad). The game's XInputGetState
// (IAT 0x61c970) reports a pad with nothing pressed while padBlock is set.
struct PadDialogState{DWORD thread;volatile LONG done;bool files=false;};   // files: the dialogs belong to an Open / Save as dialog
DWORD WINAPI padDialogThread(LPVOID context);
bool padDialogs=true;                 // [Input] PadDialogs: the controller answers the game's dialogs
bool rememberLastSave=true;           // [Game] RememberLastSave: the Load Game dialog starts on the last save used
// XInput, loaded on demand for the helper threads (the game itself uses its own XInput 9.1.0 import).
struct XInputPad{DWORD packet;WORD buttons;BYTE lt,rt;SHORT lx,ly,rx,ry;};   // XINPUT_STATE
DWORD (WINAPI* xinputGetState())(DWORD,void*) {
    static DWORD (WINAPI* function)(DWORD,void*)=nullptr;static bool tried=false;
    if(!tried){tried=true;for(const char* name:{"xinput1_4.dll","xinput1_3.dll","xinput9_1_0.dll"}){
        HMODULE module=LoadLibraryA(name);
        if(module){function=reinterpret_cast<DWORD(WINAPI*)(DWORD,void*)>(GetProcAddress(module,"XInputGetState"));if(function)break;}}}
    return function;
}
volatile LONG padBlock=0;
DWORD (WINAPI* originalXInputGetState)(DWORD,void*)=nullptr;
DWORD WINAPI xinputGetStateHook(DWORD user,void* state) {
    if(padBlock){if(state)std::memset(state,0,16);return 0;}   // XINPUT_STATE is 16 bytes; 0 = ERROR_SUCCESS
    return originalXInputGetState(user,state);
}
int __cdecl resolutionMenuHook() {
    static bool open=false;static DWORD closedAt=0;
    // Not re-entrant, and not again right after it was closed (a button still held would open it again).
    if(open || GetTickCount()-closedAt<700)return 0;
    HWND owner=at<HWND>(0x5ce858);RECT r{};
    if(!owner || !GetClientRect(owner,&r) || r.right<=0 || r.bottom<=0)return 0;
    open=true;InterlockedIncrement(&padBlock);
    PadDialogState padState{GetCurrentThreadId(),0};
    HANDLE padThread=CreateThread(nullptr,0,padDialogThread,&padState,0,nullptr);   // the pad operates the dialog
    // The list follows the shape of the picture: the whole window in widescreen, the centred 4:3 box otherwise.
    int pictureW=boxW>0?boxW:r.right,pictureH=boxH>0?boxH:r.bottom;
    ResolutionDialog dialog{pictureW,pictureH,resolutionChoices(pictureW,pictureH,renderHeight)};
    if(DialogBoxParamW(rendererModule,MAKEINTRESOURCEW(101),owner,resolutionDialogProc,reinterpret_cast<LPARAM>(&dialog))==IDOK && dialog.chosen>=0) {
        renderHeight=dialog.choices[dialog.chosen].height;
        std::wstring value=std::to_wstring(renderHeight);
        WritePrivateProfileStringW(L"Graphics",L"RenderHeight",value.c_str(),configPath.c_str());
        // Original resolution setter keeps its registry preference consistent.
        fn<void(__fastcall*)(int)>(0x45bd20)(1);
        InvalidateRect(owner,nullptr,FALSE);
    }
    InterlockedExchange(&padState.done,1);
    if(padThread){WaitForSingleObject(padThread,1000);CloseHandle(padThread);}
    InterlockedDecrement(&padBlock);open=false;closedAt=GetTickCount();
    return 0;
}
// Window management. The game opens a 4:3 window; the renderer already follows
// the client size, so a wide window only needs to be created (and Alt+Enter
// added, which the original does not offer).
bool borderlessFullscreen=false;
int windowWidth=0,windowHeight=0;     // [Window] Width/Height: client size, 0 = automatic 16:9
bool windowAltEnter=true,startFullscreen=false,desiredFullscreen=false;
RECT windowedRect{};LONG windowedStyle=0;
void logWindow(HWND window);
void setWindowedSize(HWND window) {
    MONITORINFO info{sizeof info};
    if(!GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&info))return;
    int workW=info.rcWork.right-info.rcWork.left,workH=info.rcWork.bottom-info.rcWork.top;
    LONG style=GetWindowLongA(window,GWL_STYLE),ex=GetWindowLongA(window,GWL_EXSTYLE);
    RECT frame{0,0,0,0};AdjustWindowRectEx(&frame,style,FALSE,ex);
    int extraW=frame.right-frame.left,extraH=frame.bottom-frame.top;
    int cw=windowWidth,ch=windowHeight;
    if(cw<=0 || ch<=0) {
        ch=(workH-extraH)*85/100;cw=wideGame?ch*16/9:ch*4/3;   // 4:3 when Widescreen is off
        if(cw+extraW>workW*95/100){cw=(workW-extraW)*95/100;ch=wideGame?cw*9/16:cw*3/4;}
    }
    int w=cw+extraW,h=ch+extraH;
    int x=info.rcWork.left+(workW-w)/2,y=info.rcWork.top+(workH-h)/2;
    SetWindowPos(window,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
}
void toggleFullscreen(HWND window) {
    LONG style=GetWindowLongA(window,GWL_STYLE);
    // The game may already have started in its own borderless fullscreen mode (saved "Windowed=0").
    bool fullscreenNow=borderlessFullscreen || !(style&WS_CAPTION);
    if(!fullscreenNow) {
        if(IsZoomed(window))ShowWindow(window,SW_RESTORE);
        windowedStyle=GetWindowLongA(window,GWL_STYLE);GetWindowRect(window,&windowedRect);
        MONITORINFO info{sizeof info};
        if(!GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&info))return;
        SetWindowLongA(window,GWL_STYLE,(windowedStyle&~LONG(WS_OVERLAPPEDWINDOW))|LONG(WS_POPUP));
        SetWindowPos(window,HWND_TOP,info.rcMonitor.left,info.rcMonitor.top,
                     info.rcMonitor.right-info.rcMonitor.left,info.rcMonitor.bottom-info.rcMonitor.top,
                     SWP_FRAMECHANGED|SWP_NOOWNERZORDER|SWP_SHOWWINDOW);
        borderlessFullscreen=true;
    } else {
        bool known=(windowedStyle&WS_CAPTION)!=0 && windowedRect.right>windowedRect.left;
        LONG restore=known?windowedStyle:(LONG(WS_OVERLAPPEDWINDOW)|LONG(WS_VISIBLE)|LONG(WS_CLIPSIBLINGS));
        SetWindowLongA(window,GWL_STYLE,restore);
        if(known)SetWindowPos(window,HWND_NOTOPMOST,windowedRect.left,windowedRect.top,windowedRect.right-windowedRect.left,
                              windowedRect.bottom-windowedRect.top,SWP_FRAMECHANGED|SWP_NOOWNERZORDER|SWP_SHOWWINDOW);
        else {
            SetWindowPos(window,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_FRAMECHANGED|SWP_NOOWNERZORDER|SWP_SHOWWINDOW);
            setWindowedSize(window);
        }
        borderlessFullscreen=false;
    }
    desiredFullscreen=borderlessFullscreen;
    at<uint8_t>(0x5cd47a)=borderlessFullscreen?0:1;   // the game's own "windowed" flag (videos use the window or the screen)
    InvalidateRect(window,nullptr,FALSE);logWindow(window);
}
// Alt+F4 always closes the game (not an option): ask the window to close, and end the process if the
// game's own handler does not finish within a few seconds.
DWORD WINAPI forceExitThread(LPVOID) {Sleep(3000);TerminateProcess(GetCurrentProcess(),0);return 0;}
void closeGame(HWND window) {
    static bool armed=false;
    PostMessageW(window,WM_CLOSE,0,0);
    if(!armed){armed=true;HANDLE h=CreateThread(nullptr,0,forceExitThread,nullptr,0,nullptr);if(h)CloseHandle(h);}
}
// Hotkeys (Alt+Enter / F11 fullscreen, Alt+F4 close). They normally arrive as window messages, but when the keyboard
// focus sits on another window of the game (or the messages are taken elsewhere) they never reach the game window, so a
// small helper also watches the keys themselves while the game is the foreground application. Both routes end in
// hotkeyAction, which ignores a repeat within 300 ms, so a key that arrives twice acts once.
void status(const char* message);
constexpr UINT WM_RRHD_HOTKEY=WM_APP+0x52;   // wParam: 1 = fullscreen toggle, 2 = close
void hotkeyAction(HWND window,int what,const char* route) {
    static DWORD last[3]{};static bool logged[2][3]{};
    DWORD now=GetTickCount();
    if(what<1 || what>2 || now-last[what]<300)return;
    last[what]=now;
    bool pollRoute=route[0]=='p';
    if(!logged[pollRoute][what]){logged[pollRoute][what]=true;char m[100];std::snprintf(m,sizeof m,"hotkey %s via %s",what==1?"fullscreen":"close",pollRoute?"key polling (the window did not get the key message)":"window message");status(m);}
    if(what==1){if(windowAltEnter)toggleFullscreen(window);}
    else closeGame(window);
}
DWORD WINAPI hotkeyThread(LPVOID) {
    bool wasReturn=false,wasF11=false,wasF4=false;
    for(;;) {
        Sleep(15);
        HWND foreground=GetForegroundWindow();DWORD process=0;
        if(foreground)GetWindowThreadProcessId(foreground,&process);
        HWND target=hookedWindow;
        bool mine=target && process==GetCurrentProcessId();
        bool alt=(GetAsyncKeyState(VK_MENU)&0x8000)!=0;
        bool ret=mine && alt && (GetAsyncKeyState(VK_RETURN)&0x8000)!=0,f11=mine && (GetAsyncKeyState(VK_F11)&0x8000)!=0,f4=mine && alt && (GetAsyncKeyState(VK_F4)&0x8000)!=0;
        if(ret && !wasReturn)PostMessageW(target,WM_RRHD_HOTKEY,1,0);
        if(f11 && !wasF11)PostMessageW(target,WM_RRHD_HOTKEY,1,0);
        if(f4 && !wasF4)PostMessageW(target,WM_RRHD_HOTKEY,2,0);
        wasReturn=ret;wasF11=f11;wasF4=f4;
    }
    return 0;
}
// Auto-hide the mouse cursor: hidden from the start, shown as soon as the mouse moves or a button is pressed,
// hidden again after a few seconds without mouse movement.
bool autoHideCursor=true,cursorHidden=false;
DWORD lastMouseActivity=0;POINT lastMousePoint{-1,-1};
constexpr UINT_PTR kCursorTimer=0x5252;
void showCursorAgain(HWND window) {
    if(!cursorHidden)return;
    cursorHidden=false;
    HCURSOR cursor=reinterpret_cast<HCURSOR>(GetClassLongPtrA(window,GCLP_HCURSOR));
    SetCursor(cursor?cursor:LoadCursorA(nullptr,IDC_ARROW));
}
// The game sets its own cursor all the time (a wait cursor and the arrow again when a menu or a character
// changes), which made a hidden cursor flash. While the cursor is hidden those SetCursor calls are ignored.
// A modal dialog (Save as..., message boxes) disables the game window: the cursor is shown then.
HCURSOR WINAPI setCursorHook(HCURSOR cursor) {
    if(autoHideCursor && cursorHidden && (!hookedWindow || IsWindowEnabled(hookedWindow)))return nullptr;
    return SetCursor(cursor);
}
// The cursor is hidden from the moment the game creates its window ("RashRoot"), not from the first rendered
// frame: the start-up logos and loading screens run before that.
using CreateWindowFn=HWND(WINAPI*)(DWORD,LPCSTR,LPCSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,LPVOID);
CreateWindowFn originalCreateWindow=nullptr;
void themeThread();
HWND WINAPI createWindowHook(DWORD exStyle,LPCSTR className,LPCSTR title,DWORD style,int x,int y,int w,int h,HWND parent,HMENU menu,HINSTANCE instance,LPVOID param) {
    themeThread();
    HWND window=originalCreateWindow(exStyle,className,title,style,x,y,w,h,parent,menu,instance,param);
    if(window && autoHideCursor && !IS_INTRESOURCE(className) && std::strcmp(className,"RashRoot")==0) {
        SetClassLongPtrA(window,GCLP_HCURSOR,0);   // no class cursor to bring the arrow back
        cursorHidden=true;SetCursor(nullptr);
    }
    return window;
}
// The game's own "windowed" setting (menu: Windowed / Full screen) resizes its existing window to a multiple of
// 640x480 (4:3). After it has done that, the window gets the configured size again ([Window] Width/Height, or the
// automatic 16:9 size); a switch to full screen is just remembered, so Alt+Enter keeps working.
using SetWindowPosFn=BOOL(WINAPI*)(HWND,HWND,int,int,int,int,UINT);
SetWindowPosFn originalSetWindowPos=nullptr;
BOOL WINAPI setWindowPosHook(HWND window,HWND after,int x,int y,int cx,int cy,UINT flags) {
    BOOL result=originalSetWindowPos(window,after,x,y,cx,cy,flags);
    if(result && window && window==hookedWindow && !(flags&SWP_NOSIZE)) {
        if(GetWindowLongA(window,GWL_STYLE)&WS_CAPTION) {
            borderlessFullscreen=false;desiredFullscreen=false;
            if(windowWidth>=0 && windowHeight>=0)setWindowedSize(window);
            InvalidateRect(window,nullptr,FALSE);logWindow(window);
        } else {borderlessFullscreen=true;desiredFullscreen=true;}
    }
    return result;
}
// The game has no visual-styles manifest (the exe is never modified), so its Open/Save dialogs and message boxes
// looked like Windows 95 (tiny toolbar icons). The DLL carries a Common-Controls 6 manifest and switches it on
// while such a dialog is open; the dialogs themselves stay the game's own.
HANDLE styleContext=INVALID_HANDLE_VALUE;
struct StyleScope {
    ULONG_PTR cookie=0;bool on=false;
    StyleScope() {
        if(styleContext==INVALID_HANDLE_VALUE) {
            ACTCTXW ctx{sizeof ctx};ctx.dwFlags=ACTCTX_FLAG_HMODULE_VALID|ACTCTX_FLAG_RESOURCE_NAME_VALID;
            ctx.lpResourceName=MAKEINTRESOURCEW(201);
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&styleContext),&ctx.hModule);
            styleContext=CreateActCtxW(&ctx);
        }
        if(styleContext!=INVALID_HANDLE_VALUE)on=ActivateActCtx(styleContext,&cookie)!=FALSE;
    }
    ~StyleScope(){if(on)DeactivateActCtx(0,cookie);}
};
// Kept switched on for the whole life of the game's thread, so dialogs of other DLLs shown from it (the netplay
// chooser of wsock32.dll) are themed too. Called from the places that run on the game thread.
void themeThread() {}
using OpenFileFn=BOOL(WINAPI*)(LPOPENFILENAMEA);
OpenFileFn originalOpenFile=nullptr,originalSaveFile=nullptr;
// Load Game remembers the save that was used last: the dialog opens with that file already selected. The name is kept in
// RoadRashHD.ini ([Game] LastSave) after every successful Open or Save as. While a file dialog is open the controller
// works it (see padDialogThread): D-pad up / down move through the files, A opens, B cancels.
void rememberedSaveFill(LPOPENFILENAMEA ofn) {
    if(!rememberLastSave || !ofn || !ofn->lpstrFile || ofn->nMaxFile<16 || ofn->lpstrFile[0])return;   // the game gave its own name: keep it
    wchar_t last[MAX_PATH]{};
    GetPrivateProfileStringW(L"Game",L"LastSave",L"",last,MAX_PATH,configPath.c_str());
    if(!last[0] || GetFileAttributesW(last)==INVALID_FILE_ATTRIBUTES)return;
    char ansi[MAX_PATH*2]{};
    int n=WideCharToMultiByte(CP_ACP,0,last,-1,ansi,sizeof ansi,nullptr,nullptr);
    if(n<=0 || DWORD(n)>ofn->nMaxFile)return;
    std::memcpy(ofn->lpstrFile,ansi,size_t(n));
}
void rememberChosenSave(LPOPENFILENAMEA ofn) {
    if(!rememberLastSave || !ofn || !ofn->lpstrFile || !ofn->lpstrFile[0])return;
    wchar_t chosen[MAX_PATH]{};
    if(MultiByteToWideChar(CP_ACP,0,ofn->lpstrFile,-1,chosen,MAX_PATH)<=0)return;
    wchar_t full[MAX_PATH]{};
    if(GetFullPathNameW(chosen,MAX_PATH,full,nullptr)>0)WritePrivateProfileStringW(L"Game",L"LastSave",full,configPath.c_str());
}
BOOL controlledFileDialog(OpenFileFn function,LPOPENFILENAMEA ofn,bool load) {
    StyleScope scope;
    if(load)rememberedSaveFill(ofn);
    PadDialogState state{GetCurrentThreadId(),0,true};HANDLE thread=nullptr;
    if(padDialogs){InterlockedIncrement(&padBlock);thread=CreateThread(nullptr,0,padDialogThread,&state,0,nullptr);}
    BOOL result=function(ofn);
    if(padDialogs){InterlockedExchange(&state.done,1);if(thread){WaitForSingleObject(thread,1000);CloseHandle(thread);}InterlockedDecrement(&padBlock);}
    if(result)rememberChosenSave(ofn);
    return result;
}
BOOL WINAPI getOpenFileNameHook(LPOPENFILENAMEA ofn){return controlledFileDialog(originalOpenFile,ofn,true);}
BOOL WINAPI getSaveFileNameHook(LPOPENFILENAMEA ofn){return controlledFileDialog(originalSaveFile,ofn,false);}
// Alt+F4 is also caught where the game dispatches its messages, so it closes the game whichever of its windows has the focus.
using DispatchMessageFn=LRESULT(WINAPI*)(const MSG*);
DispatchMessageFn originalDispatchMessage=nullptr;
LRESULT WINAPI dispatchMessageHook(const MSG* message) {
    if(message && message->message==WM_RRHD_HOTKEY) {hotkeyAction(message->hwnd,int(message->wParam),"poll");return 0;}
    if(message && message->message==WM_SYSKEYDOWN && message->wParam==VK_F4 && (message->lParam&(1<<29))) {
        HWND target=hookedWindow?hookedWindow:GetAncestor(message->hwnd,GA_ROOTOWNER);
        if(target){hotkeyAction(target,2,"message");return 0;}
    }
    if(message && windowAltEnter && ((message->message==WM_SYSKEYDOWN && message->wParam==VK_RETURN && (message->lParam&(1<<29))) || (message->message==WM_KEYDOWN && message->wParam==VK_F11))) {
        if(!(message->lParam&(1<<30)) && hookedWindow){hotkeyAction(hookedWindow,1,"message");return 0;}
    }
    return originalDispatchMessage(message);
}
LRESULT CALLBACK renderWindowProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    if(autoHideCursor && cursorHidden && !IsWindowEnabled(window))showCursorAgain(window);
    if(autoHideCursor) {
        switch(message) {
        case WM_MOUSEMOVE: {
            POINT p{short(LOWORD(lp)),short(HIWORD(lp))};
            if(std::abs(p.x-lastMousePoint.x)>1 || std::abs(p.y-lastMousePoint.y)>1){lastMousePoint=p;lastMouseActivity=GetTickCount();showCursorAgain(window);}
            break;}
        case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_MOUSEWHEEL:
            lastMouseActivity=GetTickCount();showCursorAgain(window);break;
        case WM_KILLFOCUS: case WM_NCMOUSEMOVE: showCursorAgain(window);break;
        case WM_SETCURSOR:
            if(cursorHidden && LOWORD(lp)==HTCLIENT){SetCursor(nullptr);return TRUE;}
            break;
        case WM_TIMER:
            if(wp==kCursorTimer) {
                if(!cursorHidden && IsWindowEnabled(window) && GetTickCount()-lastMouseActivity>=3000 && GetForegroundWindow()==window) {
                    POINT cursor{};RECT client{};
                    if(GetCursorPos(&cursor) && ScreenToClient(window,&cursor) && GetClientRect(window,&client) && PtInRect(&client,cursor)){cursorHidden=true;SetCursor(nullptr);}
                }
                return 0;
            }
            break;
        }
    }
    if(message==WM_RRHD_HOTKEY){hotkeyAction(window,int(wp),"poll");return 0;}
    if(message==WM_SYSKEYDOWN && wp==VK_F4 && (lp&(1<<29))){hotkeyAction(window,2,"message");return 0;}
    if(windowAltEnter) {
        // Alt+Enter (and F11) switch between the window and borderless fullscreen.
        if((message==WM_SYSKEYDOWN && wp==VK_RETURN && (lp&(1<<29))) || (message==WM_KEYDOWN && wp==VK_F11)) {
            if(!(lp&(1<<30)))hotkeyAction(window,1,"message"); // ignore auto-repeat
            return 0;
        }
        if(message==WM_SYSCHAR && wp==VK_RETURN)return 0; // no system beep
    }
    if(message==WM_ERASEBKGND && completedFrame)return 1;
    WNDPROC previous=previousWindowProc;
    LRESULT result=CallWindowProcA(previous,window,message,wp,lp);
    if(message==WM_NCDESTROY){hookedWindow=nullptr;previousWindowProc=nullptr;completedFrame=false;}
    return result;
}
void logWindow(HWND window);
void hookWindow(HWND window) {
    themeThread();
    {static bool started=false;if(!started && window){started=true;HANDLE h=CreateThread(nullptr,0,hotkeyThread,nullptr,0,nullptr);if(h)CloseHandle(h);}}
    if(!window || hookedWindow==window)return;
    auto previous=reinterpret_cast<WNDPROC>(SetWindowLongPtrA(window,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&renderWindowProc)));
    if(previous){hookedWindow=window;previousWindowProc=previous;}
    if(previous && autoHideCursor) {
        // Start hidden. The mouse position at this moment counts as "not moved" (Windows sends a synthetic
        // WM_MOUSEMOVE when a window appears under the cursor), so only real movement brings the cursor back.
        POINT cursor{};RECT client{};
        lastMouseActivity=GetTickCount();
        if(GetCursorPos(&cursor) && ScreenToClient(window,&cursor)){lastMousePoint=cursor;
            if(GetClientRect(window,&client) && PtInRect(&client,cursor)){cursorHidden=true;SetCursor(nullptr);}}
        SetTimer(window,kCursorTimer,300,nullptr);
    }
    if(previous) {
        // Applied to every window the game creates (it re-creates its window when it changes mode).
        static HWND placedWindow=nullptr;
        if(placedWindow!=window) {
            bool first=placedWindow==nullptr;
            placedWindow=window;borderlessFullscreen=false;
            if(first)desiredFullscreen=startFullscreen;
            if(!IsZoomed(window) && (GetWindowLongA(window,GWL_STYLE)&WS_OVERLAPPEDWINDOW)==WS_OVERLAPPEDWINDOW) {
                if(windowWidth>=0 && windowHeight>=0)setWindowedSize(window);
            }
            if(desiredFullscreen) {
                // The game may already have created this window as a borderless popup: do not toggle it back.
                if(GetWindowLongA(window,GWL_STYLE)&WS_CAPTION)toggleFullscreen(window);
                else borderlessFullscreen=true;
            }
            desiredFullscreen=borderlessFullscreen||desiredFullscreen;
            at<uint8_t>(0x5cd47a)=(GetWindowLongA(window,GWL_STYLE)&WS_CAPTION)?1:0;
        }
    }
    {
        char current[256]{};GetWindowTextA(window,current,sizeof current);
        if(std::strncmp(current,"Road Rash",9)==0 && std::strcmp(current,RRHD_WINDOW_TITLE)!=0)SetWindowTextA(window,RRHD_WINDOW_TITLE);
    }
    logWindow(window);
}

struct RasterJob {Vertex points[4];Texture texture;uint8_t color;size_t snapshot=SIZE_MAX;Vertex legacy[4];bool hasLegacy=false;};
std::vector<RasterJob> rasterJobs,frameJobs;
// The indexed world raster (1 B per pixel) belongs to the CPU renderer only. With Direct3D it would just sit in
// memory (about 8 MB at 4K), so it is created on first use.
bool cpuWorld=false;
void ensureCpuWorld();
std::vector<uint8_t> textureSnapshots;
void enqueueQuad(const Vertex (&points)[4],Texture texture,uint8_t color,bool changing=false,const Vertex* logical=nullptr) {
    RasterJob job{};std::memcpy(job.points,points,sizeof job.points);job.texture=texture;job.color=color;
    if(logical){for(int i=0;i<4;++i)job.legacy[i]={logical[i].x,logical[i].y,points[i].u,points[i].v};job.hasLegacy=true;}
    if(changing && texture.pixels) {
        double ul=points[0].u,ur=ul,vt=points[0].v,vb=vt;
        for(const auto& p:points){ul=std::min(ul,p.u);ur=std::max(ur,p.u);vt=std::min(vt,p.v);vb=std::max(vb,p.v);}
        int x0=std::clamp(int(ul),0,texture.width-1),x1=std::clamp(int(ur)+1,1,texture.width);
        int y0=std::clamp(int(vt),0,texture.height-1),y1=std::clamp(int(vb)+1,1,texture.height);
        job.snapshot=textureSnapshots.size();job.texture.width=x1-x0;job.texture.height=y1-y0;
        for(auto& p:job.points){p.u-=x0;p.v-=y0;}
        for(auto& p:job.legacy){p.u-=x0;p.v-=y0;}
        // The actor can occupy only a small rectangle in a large animation
        // atlas. Snapshot that rectangle, not every unused animation frame.
        for(int y=y0;y<y1;++y) {
            auto row=texture.pixels+size_t(y)*texture.width+x0;
            textureSnapshots.insert(textureSnapshots.end(),row,row+(x1-x0));
        }
    }
    rasterJobs.push_back(job);
}
// Disjoint row strips preserve the original draw order at every pixel. Sprite
// images modified by the engine are snapshotted before workers start reading.
void (*workerRows)(int,int)=nullptr;   // when set, the workers run this instead of drawing quads
struct RasterWorkers {
    struct Strip {int index;RasterWorkers* owner;PTP_WORK work;};
    std::array<Strip,64> strips{};int count=0;
    static void CALLBACK render(PTP_CALLBACK_INSTANCE,void* context,PTP_WORK) {
        auto s=static_cast<Strip*>(context);
        int top=height*s->index/s->owner->count,bottom=height*(s->index+1)/s->owner->count;
        if(workerRows){workerRows(top,bottom);return;}
        for(const auto& job:rasterJobs)rrhd::quad(world.data(),width,height,job.points,job.texture,job.color,top,bottom);
    }
    void initialize() {
        // Several strips per core: the bottom of the screen (road, close sprites) is far
        // heavier than the top, so small strips let the pool balance the load.
        count=std::max(2,std::min(64,int(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS))*2));
        for(int i=0;i<count;++i) {strips[i]={i,this,nullptr};strips[i].work=CreateThreadpoolWork(render,&strips[i],nullptr);}
    }
    ~RasterWorkers() {for(auto& s:strips)if(s.work)CloseThreadpoolWork(s.work);}
} rasterWorkers;
void runRasterJobs() {
    for(auto& job:rasterJobs)if(job.snapshot!=SIZE_MAX)job.texture.pixels=textureSnapshots.data()+job.snapshot;
    if(rasterJobs.size()<32 || height<480) {
        for(const auto& job:rasterJobs)rrhd::quad(world.data(),width,height,job.points,job.texture,job.color,0,0x7fffffff);
    } else {
        if(!rasterWorkers.count)rasterWorkers.initialize();
        for(int i=0;i<rasterWorkers.count;++i) {
            auto& s=rasterWorkers.strips[i];
            if(s.work)SubmitThreadpoolWork(s.work);else RasterWorkers::render(nullptr,&s,nullptr);
        }
        for(int i=0;i<rasterWorkers.count;++i)if(rasterWorkers.strips[i].work)
            WaitForThreadpoolWorkCallbacks(rasterWorkers.strips[i].work,FALSE);
    }
    rasterJobs.clear();textureSnapshots.clear();
}
// Run fn(top,bottom) over disjoint output-row strips on the worker pool.
void parallelRows(void (*fn)(int,int)) {
    if(height<480){fn(0,height);return;}
    if(!rasterWorkers.count)rasterWorkers.initialize();
    workerRows=fn;
    for(int i=0;i<rasterWorkers.count;++i) {
        auto& s=rasterWorkers.strips[i];
        if(s.work)SubmitThreadpoolWork(s.work);else RasterWorkers::render(nullptr,&s,nullptr);
    }
    for(int i=0;i<rasterWorkers.count;++i)if(rasterWorkers.strips[i].work)
        WaitForThreadpoolWorkCallbacks(rasterWorkers.strips[i].work,FALSE);
    workerRows=nullptr;
}

void status(const char* message) {
    if(logPath.empty()) return;
    FILE* f=nullptr;
    if(_wfopen_s(&f,logPath.c_str(),L"a")==0 && f) { std::fprintf(f,"%s\n",message);std::fclose(f); }
}
// Optional frame-time profiling (RoadRashHD.ini: Profile=1). Logs every five
// seconds: average time per stage and the distribution of presented-frame
// intervals, which is what a viewer perceives as stutter.
bool profileEnabled=false;
bool gpuFrame=false,fixPostcards=true,fixBikeColors=true,keyboardAndPad=true,keyboardAndPadActive=false;
int videoVolume=100;   // [Game] VideoVolume, percent
bool asyncControllerScan=true,fixTextEncoding=true,skipIntro=false,fullRateObjects=true;
int dumpEverySeconds=0,dumpScale=2;
void logWindow(HWND window) {
    if(!profileEnabled)return;
    RECT w{},c{};GetWindowRect(window,&w);GetClientRect(window,&c);
    char m[300];std::snprintf(m,sizeof m,"window: style=%08lx ex=%08lx rect=%ld,%ld %ldx%ld client=%ldx%ld zoomed=%d menu=%d",
        GetWindowLongA(window,GWL_STYLE),GetWindowLongA(window,GWL_EXSTYLE),w.left,w.top,w.right-w.left,w.bottom-w.top,c.right,c.bottom,IsZoomed(window)?1:0,GetMenu(window)?1:0);
    status(m);
}
enum Stage {StageBackdrop,StageRaster,StageLegacy,StageCompose,StagePresent,StageMenu,StageMenuBackdrop,StageGpu,StageCount};
// Stall sampler (profile mode only): while the game loop has not presented for
// 25+ ms, briefly suspend the game thread and log where it is waiting.
HANDLE watchdogMain=nullptr;
volatile LONGLONG watchdogLast=0;
std::wstring describeAddress(uintptr_t address) {
    HMODULE module=nullptr;wchar_t name[MAX_PATH]{};
    if(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(address),&module) && GetModuleFileNameW(module,name,MAX_PATH)) {
        std::wstring path(name);path=path.substr(path.find_last_of(L"\\/")+1);
        wchar_t out[96];swprintf(out,96,L"%ls+0x%zx",path.c_str(),address-reinterpret_cast<uintptr_t>(module));
        return out;
    }
    wchar_t out[32];swprintf(out,32,L"0x%zx",address);return out;
}
bool looksLikeReturn(uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof info) || info.Type!=MEM_IMAGE)return false;
    if(!(info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
    auto b=reinterpret_cast<const uint8_t*>(address);
    return b[-5]==0xe8 || b[-2]==0xff || b[-6]==0xff;
}
DWORD WINAPI stallWatchdog(LPVOID) {
    LARGE_INTEGER f;QueryPerformanceFrequency(&f);
    unsigned logged=0;LONGLONG stall=0;int samples=0;
    for(;;) {
        Sleep(6);
        LONGLONG last=InterlockedCompareExchange64(&watchdogLast,0,0);
        if(!last || logged>=90)continue;
        LARGE_INTEGER n;QueryPerformanceCounter(&n);LONGLONG gap=n.QuadPart-last;
        if(last!=stall){stall=last;samples=0;}
        if(gap>f.QuadPart*2)continue;
        if(gap<f.QuadPart*(25+samples*35)/1000 || samples>=4)continue;
        CONTEXT c{};c.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
        uintptr_t words[128]{};SIZE_T got=0;uintptr_t eip=0;bool ok=false;
        if(SuspendThread(watchdogMain)==DWORD(-1))continue;
        if(GetThreadContext(watchdogMain,&c)) {
            eip=c.Eip;ok=true;
            ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(c.Esp),words,sizeof words,&got);
        }
        ResumeThread(watchdogMain);
        ++samples;if(!ok)continue;
        std::wstring line=L"stall sample at +"+std::to_wstring(int(gap*1000/f.QuadPart))+L" ms: EIP "+describeAddress(eip)+L"; callers:";
        int depth=0;
        for(size_t i=0;i<got/4 && depth<7;++i)
            if(looksLikeReturn(words[i])){line+=L" "+describeAddress(words[i]);++depth;}
        char narrow[900];
        WideCharToMultiByte(CP_UTF8,0,line.c_str(),-1,narrow,sizeof narrow,nullptr,nullptr);
        status(narrow);++logged;
    }
}
void startWatchdog() {
    if(watchdogMain)return;
    if(DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&watchdogMain,
                       THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,0)) {
        HANDLE h=CreateThread(nullptr,0,stallWatchdog,nullptr,0,nullptr);if(h)CloseHandle(h);
    }
}
// Diagnostics: DumpFrameEverySeconds=N saves up to 16 half-size BMPs of the
// presented frame (RoadRashHD_frame_NN.bmp) so a headless run can be inspected.
void dumpFrame(LONGLONG nowTicks) {
    static LONGLONG next=0,freq=0;static int saved=0;
    if(dumpEverySeconds<=0 || saved>=48 || rgba.empty())return;
    const uint32_t* source=rgba.data();
    if(gpuRequested && gpu.ready()){if(!gpu.readScene(gpuShot))return;source=gpuShot.data();}
    if(!freq){LARGE_INTEGER f;QueryPerformanceFrequency(&f);freq=f.QuadPart;next=nowTicks+freq*dumpEverySeconds;}
    if(nowTicks<next)return;
    next=nowTicks+freq*dumpEverySeconds;
    int scale=std::max(1,dumpScale);int w=width/scale,h=height/scale;if(w<=0||h<=0)return;
    std::vector<uint32_t> half(size_t(w)*h);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x)half[size_t(y)*w+x]=source[size_t(y*scale)*width+x*scale];
    BITMAPFILEHEADER fh{};BITMAPINFOHEADER ih{};
    ih.biSize=sizeof ih;ih.biWidth=w;ih.biHeight=-h;ih.biPlanes=1;ih.biBitCount=32;ih.biCompression=BI_RGB;
    fh.bfType=0x4d42;fh.bfOffBits=sizeof fh+sizeof ih;fh.bfSize=fh.bfOffBits+DWORD(half.size()*4);
    std::wstring path=logPath.substr(0,logPath.find_last_of(L'\\')+1)+L"RoadRashHD_frame_"+std::to_wstring(saved)+L".bmp";
    FILE* f=nullptr;
    if(_wfopen_s(&f,path.c_str(),L"wb")==0 && f){fwrite(&fh,sizeof fh,1,f);fwrite(&ih,sizeof ih,1,f);fwrite(half.data(),4,half.size(),f);fclose(f);}
    ++saved;
}
// Texture filtering exists only in the Direct3D renderer; the CPU/GDI path draws nearest-neighbour.
bool configFilter=true;
bool gameplayFramePresented=false;
struct Profiler {
    LONGLONG stage[StageCount]{},lastPresent=0,reportStart=0,frequency=0,worstRebuild=0;
    unsigned rebuilds=0,slowLogged=0;
    LONGLONG lastExit=0,lastStretch=0,maxStretch=0;
    // Called once per presented menu frame with the entry, post-compose and exit times.
    void frame(LONGLONG enter,LONGLONG composed,LONGLONG exit) {
        if(!frequency){LARGE_INTEGER f;QueryPerformanceFrequency(&f);frequency=f.QuadPart;}
        if(lastExit && enter-lastExit>frequency/25 && enter-lastExit<frequency*2 && slowLogged<60) {
            char message[300];
            std::snprintf(message,sizeof message,
                "slow frame: game loop gap %.1f ms before present; our compose %.1f ms; original present %.1f ms (StretchDIBits %.1f ms)",
                (enter-lastExit)*1000.0/frequency,(composed-enter)*1000.0/frequency,(exit-composed)*1000.0/frequency,lastStretch*1000.0/frequency);
            status(message);++slowLogged;
        }
        lastExit=exit;
        startWatchdog();InterlockedExchange64(&watchdogLast,exit);
    }
    std::vector<float> intervals;
    LONGLONG now() const {LARGE_INTEGER v;QueryPerformanceCounter(&v);return v.QuadPart;}
    void add(Stage s,LONGLONG begin) {if(profileEnabled)stage[s]+=now()-begin;}
    void presented(LONGLONG end) {
        if(!frequency){LARGE_INTEGER f;QueryPerformanceFrequency(&f);frequency=f.QuadPart;}
        if(lastPresent && end-lastPresent<frequency/2 && intervals.size()<4096)intervals.push_back(float((end-lastPresent)*1000.0/frequency));
        lastPresent=end;
        if(!reportStart)reportStart=end;
        if(end-reportStart<frequency*5)return;
        if(!intervals.empty()) {
            auto sorted=intervals;std::sort(sorted.begin(),sorted.end());
            size_t n=sorted.size();unsigned slow=0,veryslow=0;
            for(float v:sorted){if(v>20.0f)++slow;if(v>33.0f)++veryslow;}
            char gpuTimeText[120]="gpu time n/a";
            {   // GPU milliseconds per frame (scene + UI + scaling), measured with timestamp queries
                auto times=gpu.takeGpuTimes();
                if(!times.empty()){
                    std::sort(times.begin(),times.end());double sum=0;for(float v:times)sum+=v;
                    std::snprintf(gpuTimeText,sizeof gpuTimeText,"gpu ms/frame avg %.2f p95 %.2f max %.2f (%zu samples)",sum/times.size(),times[std::min(times.size()-1,times.size()*95/100)],times.back(),times.size());
                }
            }
            char message[420];
            std::snprintf(message,sizeof message,
                "profile %dx%d: frames %zu; interval ms p50 %.1f p95 %.1f p99 %.1f max %.1f; >20ms %u, >33ms %u; "
                "avg ms/frame: backdrop %.2f raster %.2f legacy-copy %.2f compose %.2f present %.2f menu %.2f; "
                "menu backdrop rebuilds %u (worst %.1f ms, total %.1f ms); gpu-submit %.2f; %s",
                width,height,n,sorted[n/2],sorted[std::min(n-1,n*95/100)],sorted[std::min(n-1,n*99/100)],sorted[n-1],slow,veryslow,
                stage[StageBackdrop]*1000.0/frequency/n,stage[StageRaster]*1000.0/frequency/n,
                stage[StageLegacy]*1000.0/frequency/n,stage[StageCompose]*1000.0/frequency/n,stage[StagePresent]*1000.0/frequency/n,
                stage[StageMenu]*1000.0/frequency/n,rebuilds,worstRebuild*1000.0/frequency,stage[StageMenuBackdrop]*1000.0/frequency,
                stage[StageGpu]*1000.0/frequency/n,gpuTimeText);
            status(message);
        }
        intervals.clear();std::memset(stage,0,sizeof stage);rebuilds=0;worstRebuild=0;reportStart=end;
    }
} profiler;
uint8_t* __fastcall animationFrameHook(uint8_t* animation,int sequence,int frame,int* duration) {
    auto resource=*reinterpret_cast<const uint8_t* const*>(animation);
    unsigned kind=*reinterpret_cast<const unsigned*>(resource+8);
    // 0x41c700 returns null for resource kinds outside 1..4. The original
    // frame selector nevertheless dereferences that result at 0x41bed2.
    // Its caller already handles a null frame by skipping that actor's draw.
    if(kind<1 || kind>4) {
        if(duration)*duration=0;
        static bool reported=false;
        if(!reported) {
            char message[160];std::snprintf(message,sizeof message,
                "missing animation metadata: kind=%u sequence=%d frame=%d; skipped unsafe frame",kind,sequence,frame);
            status(message);reported=true;
        }
        return nullptr;
    }
    return fn<AnimationFrame>(0x41be80)(animation,sequence,frame,duration);
}
bool writeMemory(uintptr_t address,const void* bytes,size_t length) {
    void* dest=reinterpret_cast<void*>(address);DWORD oldProtect;
    if(!VirtualProtect(dest,length,PAGE_EXECUTE_READWRITE,&oldProtect)) return false;
    std::memcpy(dest,bytes,length);FlushInstructionCache(GetCurrentProcess(),dest,length);
    DWORD ignored;VirtualProtect(dest,length,oldProtect,&ignored);return true;
}
bool branch(uintptr_t va,void* target,uint8_t opcode) {
    uint8_t code[5]={opcode};uint32_t rel=uint32_t(reinterpret_cast<uintptr_t>(target)-(base+va-0x400000+5));
    std::memcpy(code+1,&rel,4);return writeMemory(base+va-0x400000,code,sizeof code);
}
bool isBranch(uintptr_t va,uint8_t opcode,uintptr_t target) {
    return at<uint8_t>(va)==opcode && uintptr_t(intptr_t(va+5)+at<int32_t>(va+1))==target;
}
// Replaces one of the game's imports (the slot at va of its import table). With a module and name given, the slot
// must still point at that function; the previous value is returned through original.
bool hookImport(uintptr_t va,const wchar_t* module,const char* name,void* hook,void** original=nullptr) {
    void* current=at<void*>(va);
    if(module && current!=reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(module),name)))return false;
    if(original)*original=current;
    return writeMemory(base+va-0x400000,&hook,sizeof hook);
}

// Physical client dimensions, without an aspect whitelist or fixed row table.
void viewport(int cw,int ch) {
    clientWidth=cw;clientHeight=ch;
    int bw=cw,bh=ch,bx=0,by=0;
    if(!wideGame) {   // original 4:3 picture: the largest 4:3 box that fits, centred
        if(cw*3>ch*4){bw=std::max(1,ch*4/3);bx=(cw-bw)/2;}
        else if(cw*3<ch*4){bh=std::max(1,cw*3/4);by=(ch-bh)/2;}
    }
    boxX=bx;boxY=by;boxW=bw;boxH=bh;
    int h=renderHeight>0?renderHeight:bh;
    int w=renderHeight>0?std::max(1,int(std::lround(double(bw)*h/bh))):bw;
    logicalWidth=(int(std::ceil(240.0*w/h))+1)&~1;
    sceneScale=h/480.0;sceneOffset=w/2.0-logicalWidth*sceneScale;
    if(w==width && h==height) return;
    width=w;height=h;
    rgba.resize(size_t(w)*h);
    if(cpuWorld)world.resize(size_t(w)*h);
    baseline.resize(640*480);beforeText.resize(640*480);
    mapX.resize(w);mapY.resize(h);sceneX.resize(640);sceneY.resize(480);
    backdropX.resize(w);panoramaX.resize(w);menuBackdropDirty=true;
    double ui=std::min(w/640.0,h/480.0);
    double ox=(w-640*ui)/2,oy=(h-480*ui)/2;
    for(int x=0;x<=640;++x)uiX[x]=std::clamp(int(std::ceil(ox+x*ui-0.5)),0,w);
    for(int y=0;y<=480;++y)uiY[y]=std::clamp(int(std::ceil(oy+y*ui-0.5)),0,h);
    for(int x=0;x<w;++x) {
        int p=int(std::floor((x+0.5-ox)/ui));mapX[x]=(p>=0 && p<640)?p:-1;
        backdropX[x]=int(std::floor((x+0.5-w/2.0)/sceneScale+320));
    }
    for(int y=0;y<h;++y) {
        int p=int(std::floor((y+0.5-oy)/ui));mapY[y]=(p>=0 && p<480)?p:-1;
    }
    menuCacheValid=false;
    if(smoothUi) {   // bilinear sampling positions of the menu picture (8-bit weights)
        auto fill=[ui](std::vector<int>& i0,std::vector<int>& i1,std::vector<unsigned>& weight,int count,double origin,int size) {
            i0.resize(count);i1.resize(count);weight.resize(count);
            for(int i=0;i<count;++i) {
                double p=(i+0.5-origin)/ui-0.5;int a=int(std::floor(p));unsigned wgt=unsigned(std::lround((p-a)*256.0));
                if(a<0){a=0;wgt=0;}
                if(a>=size-1){a=size-1;wgt=0;}
                i0[i]=a;i1[i]=std::min(a+1,size-1);weight[i]=std::min(256u,wgt);
            }
        };
        fill(smoothX0,smoothX1,smoothWX,w,ox,640);fill(smoothY0,smoothY1,smoothWY,h,oy,480);
    }
    for(int x=0;x<640;++x) sceneX[x]=int(std::floor((x+0.5-320)*sceneScale+w/2.0));
    for(int y=0;y<480;++y) sceneY[y]=std::min(h-1,int((y+0.5)*sceneScale));
    char msg[160];std::snprintf(msg,sizeof msg,"native viewport %dx%d; logical camera %dx240; uniform scale %.6f",w,h,logicalWidth,sceneScale);status(msg);
}
bool windowViewport() {
    RECT r{};HWND hwnd=at<HWND>(0x5ce858);
    if(hwnd && GetClientRect(hwnd,&r)) {
        if(r.right<=0 || r.bottom<=0) return false;
        hookWindow(hwnd);
        viewport(r.right,r.bottom);
    } else viewport(GetSystemMetrics(SM_CXSCREEN),GetSystemMetrics(SM_CYSCREEN));
    return true;
}
bool highMode() {
    return enabled && at<uint8_t>(0x5a292a) && at<int>(0x5a2934)==640 && at<int>(0x5a2938)==480;
}
void ensureCpuWorld() {
    if(cpuWorld)return;
    cpuWorld=true;world.assign(size_t(width)*height,0);
}
void startFrame() {
    frameActive=true;begun=false;worldReady=false;displayReady=false;gpuSceneReady=false;gpuFrame=false;
    primitives=0;batches=0;frameJobs.clear();textureSnapshots.clear();
}
// A profile that was set up for the keyboard has no pad codes in the game's assignment list (19 entries of one pad
// code and one key code at 0x590e18; the game finds out which device is in use by looking for any pad code), so the
// pad had nothing to press. Missing pad codes are filled in with the game's own defaults, and the list is used in
// full (the pad-only D-pad entries are past the keyboard's 15). Codes the profile already has are never changed.
void ensurePadAssignments() {
    static const uint8_t defaults[19]={0x08,0x07,0x28,0x05,0x01,0x03,0x02,0x04,0x0c,0x26,0x06,0,0,0,0,0x25,0x27,0x28,0x26};
    static bool logged=false;
    bool changed=false;
    for(int i=0;i<19;++i) {
        uint8_t& code=at<uint8_t>(0x590e18+2*i);
        if(code==0 && defaults[i]){code=defaults[i];changed=true;}
    }
    int& count=at<int>(0x5ce83c);
    if(count>0 && count<19){count=19;changed=true;}
    if(changed && !logged){logged=true;status("input: pad assignments added to a keyboard profile");}
}
int __cdecl frameHook() {
    if(keyboardAndPadActive)ensurePadAssignments();
    if(!highMode() || !windowViewport()) { worldReady=false;displayReady=false;return fn<DrawList>(0x45ac50)(); }
    LARGE_INTEGER begin{},end{},frequency{};QueryPerformanceCounter(&begin);
    startFrame();int previous=at<int>(0x591c0c);
    // Projection centre AND scene culling, only during the rendering phase.
    at<int>(0x591c0c)=logicalWidth;
    int result=fn<DrawList>(0x45ac50)();
    at<int>(0x591c0c)=previous;frameActive=false;
    QueryPerformanceCounter(&end);QueryPerformanceFrequency(&frequency);
    static LONGLONG interval=0,total=0,lastEnd=0;static unsigned samples=0;
    if(!interval || begin.QuadPart-lastEnd>frequency.QuadPart/2) {interval=begin.QuadPart;total=0;samples=0;}
    lastEnd=end.QuadPart;
    total+=end.QuadPart-begin.QuadPart;++samples;
    if(end.QuadPart-interval>=frequency.QuadPart*5) {
        char message[180];
        std::snprintf(message,sizeof message,"render cadence %.2f fps; rendering %.3f ms/frame; %dx%d; %u samples",
            samples*double(frequency.QuadPart)/(end.QuadPart-interval),total*1000.0/frequency.QuadPart/samples,width,height,samples);
        status(message);interval=end.QuadPart;total=0;samples=0;
    }
    return result;
}
int __cdecl worldPresentHook() {
    int previous=at<int>(0x591c0c);
    if(frameActive) at<int>(0x591c0c)=320;
    int result=fn<DrawList>(0x467b80)();
    at<int>(0x591c0c)=previous;return result;
}

int __fastcall horizonLoadHook(const char* path) {
    int result=fn<HorizonLoad>(0x468560)(path);
    horizon.clear();horizonWidth=at<int>(0x5ce340);horizonRows=0;
    FILE* f=nullptr;
    if(path && horizonWidth>0 && fopen_s(&f,path,"rb")==0 && f) {
        std::fseek(f,0,SEEK_END);long size=std::ftell(f);std::rewind(f);
        if(size>0) {
            horizonRows=int(size/horizonWidth);horizon.resize(size_t(horizonRows)*horizonWidth);
            if(std::fread(horizon.data(),1,horizon.size(),f)!=horizon.size()) {horizon.clear();horizonRows=0;}
        }
        std::fclose(f);
    }
    gpuHorizonDirty=true;
    if(gpu.ready())gpu.flushTextures();
    return result;
}
void __fastcall horizonDrawHook(int scroll,int y) {
    horizonScroll=scroll;horizonY=y;fn<HorizonDraw>(0x469770)(scroll,y);
}
int wrap(int x,int n) { int r=x%n;return r<0?r+n:r; }
void paintBackdrop(const uint8_t* pixels,int pitch) {
    if(horizonRows>0)for(int x=0;x<width;++x)panoramaX[x]=wrap(backdropX[x]-horizonScroll,horizonWidth);
    int previous=-1;
    for(int y=0;y<height;++y) {
        int oldY=std::clamp(int((y+0.5)/sceneScale),0,479);
        uint8_t* out=world.data()+size_t(y)*width;
        if(oldY==previous) {std::memcpy(out,out-width,width);continue;}
        previous=oldY;
        int hy=oldY-horizonY;
        if(horizonRows>0 && hy>=0 && hy<horizonRows) {
            auto src=horizon.data()+size_t(hy)*horizonWidth;
            for(int x=0;x<width;++x)out[x]=src[panoramaX[x]];
        } else {
            auto src=pixels+size_t(oldY)*pitch;
            for(int x=0;x<width;++x)out[x]=src[std::clamp(backdropX[x],0,639)];
        }
    }
}
// ---- Direct3D helpers -------------------------------------------------------------------
bool ensureGpu() {
    if(!gpuRequested || gpu.failed || width<=0 || height<=0)return false;
    HWND hwnd=at<HWND>(0x5ce858);
    if(!hwnd || !IsWindow(hwnd))return false;
    if(!gpu.ready() || gpu.windowHandle()!=hwnd) {
        // First use, after a video, or the game re-created its window (it does so when it switches
        // between window and fullscreen): bind the swap chain to the window that is on screen now.
        if(gpu.windowHandle() && gpu.windowHandle()!=hwnd)status("Direct3D: the game window changed, re-binding the swap chain");
        if(!gpu.init(hwnd,width,height))return false;
        gpuHorizonDirty=true;
    } else if(!gpu.resizeScene(width,height))return false;
    return gpu.ready();
}
void fixSnapshotPointers(std::vector<RasterJob>& jobs) {
    for(auto& job:jobs)if(job.snapshot!=SIZE_MAX)job.texture.pixels=textureSnapshots.data()+job.snapshot;
}
// Gameplay: move the new quads to the frame list and update the 640x480 picture the game draws its HUD on.
void collectGpuJobs() {
    size_t first=frameJobs.size();
    frameJobs.insert(frameJobs.end(),rasterJobs.begin(),rasterJobs.end());
    rasterJobs.clear();
    fixSnapshotPointers(frameJobs);
    for(size_t i=first;i<frameJobs.size();++i) {
        const auto& job=frameJobs[i];
        if(job.hasLegacy)rrhd::quad(legacyPicture.data(),640,480,job.legacy,job.texture,job.color);
    }
    auto pixels=at<uint8_t*>(0x6036bc);int pitch=at<int>(0x6036b8);
    for(int y=0;y<480;++y) {
        std::memcpy(pixels+size_t(y)*pitch,legacyPicture.data()+size_t(y)*640,640);
        std::memcpy(baseline.data()+size_t(y)*640,legacyPicture.data()+size_t(y)*640,640);
    }
    worldReady=true;gpuSceneReady=true;
}
bool beginWorld() {
    if(begun) return true;
    if(!frameActive || !at<uint8_t>(0x5ce34c)) return false;
    auto pixels=at<const uint8_t*>(0x6036bc);int pitch=at<int>(0x6036b8);
    if(!pixels || pitch<640) return false;
    if(ensureGpu()) {
        // Direct3D frame: remember the game's picture before any quad is drawn (sky, panorama source).
        for(int y=0;y<480;++y)std::memcpy(legacyBackdrop.data()+size_t(y)*640,pixels+size_t(y)*pitch,640);
        legacyPicture=legacyBackdrop;gpuFrame=true;begun=true;return true;
    }
    gpuFrame=false;
    ensureCpuWorld();
    LONGLONG t0=profileEnabled?profiler.now():0;
    paintBackdrop(pixels,pitch);profiler.add(StageBackdrop,t0);begun=true;return true;
}
Vertex vertex(double x,double y,double u=0,double v=0) {
    return {x*sceneScale+sceneOffset,y*sceneScale,u,v};
}
void drawNode(uint8_t* node) {
    auto resource=*reinterpret_cast<uint8_t**>(node+4);
    if(!resource) return;
    int flags=*reinterpret_cast<int*>(resource+12);
    const auto source=reinterpret_cast<const int32_t*>(node-0x68+0x44);
    Vertex p[4];
    for(int i=0;i<4;++i)p[i]={source[i*2]/32768.0,source[i*2+1]/32768.0};
    bool flat=(flags & at<int>(0x5a29f4))!=0,flipX=node[61]!=0;
    if(!rrhd::normalize(p,flat?0:flags,flipX) && !flat)return;
    node[61]=uint8_t(flipX);
    Vertex logical[4];
    for(int i=0;i<4;++i)logical[i]={p[i].x+320.0-logicalWidth,p[i].y};
    for(auto& v:p)v=vertex(v.x,v.y);
    if(flat) {
        uint8_t color=*reinterpret_cast<int*>(resource)==16?node[60]:resource[44];
        enqueueQuad(p,{},color,false,logical);++primitives;return;
    }
    if(flags & 0x2000) fn<Actor>(0x458f30)(node);
    resource=*reinterpret_cast<uint8_t**>(node+4);
    auto bounds=reinterpret_cast<const int*>(node+8);
    int tw=*reinterpret_cast<int*>(resource+32),th=*reinterpret_cast<int*>(resource+36);
    int levels=*reinterpret_cast<int*>(resource+40);
    if(tw<=0 || tw>256 || th<=0 || levels<=0 || levels>16) return;
    double ul=node[61]?bounds[2]-1:bounds[0]+1,ur=node[61]?bounds[0]+1:bounds[2]-1;
    double vt=node[62]?bounds[3]-1:bounds[1]+1,vb=node[62]?bounds[1]+1:bounds[3]-1;
    p[0].u=ul;p[0].v=vt;p[1].u=ur;p[1].v=vt;
    p[2].u=ur;p[2].v=vb;p[3].u=ul;p[3].v=vb;
    double xmin=p[0].x,xmax=xmin,ymin=p[0].y,ymax=ymin;
    for(const auto& v:p) {xmin=std::min(xmin,v.x);xmax=std::max(xmax,v.x);ymin=std::min(ymin,v.y);ymax=std::max(ymax,v.y);}
    auto choose=[levels](double dst,int src) {
        int l=0;double size=(std::abs(src)+1)*0.5;
        while(l+1<levels && dst<size){++l;size*=0.5;}return l;
    };
    int mip=std::min(choose(xmax-xmin+1,bounds[2]-bounds[0]),choose(ymax-ymin+1,bounds[3]-bounds[1]));
    auto data=*reinterpret_cast<const uint8_t* const*>(resource+56+12*mip);
    if(data) {
        double divisor=double(1<<mip);for(auto& v:p){v.u/=divisor;v.v/=divisor;}
        Texture t{data,std::max(1,tw>>mip),std::max(1,th>>mip),(flags&2)!=0};
        enqueueQuad(p,t,0,(flags&0x2000)!=0,logical);++primitives;
    }
}
void copyLegacyView() {
    auto pixels=at<uint8_t*>(0x6036bc);int pitch=at<int>(0x6036b8);
    for(int y=0;y<480;++y)for(int x=0;x<640;++x) {
        int sx=sceneX[x];uint8_t c=(sx>=0 && sx<width)?world[size_t(sceneY[y])*width+sx]:0;
        pixels[size_t(y)*pitch+x]=c;baseline[y*640+x]=c;
    }
    worldReady=true;
}
int __cdecl listHook() {
    if(!frameActive) return fn<DrawList>(0x4688e0)();
    // Retain DirectDraw locking, bypass the legacy 640-pixel cull and raster.
    auto head=at<uint8_t*>(0x6185e0);at<uint8_t*>(0x6185e0)=nullptr;
    int result=fn<DrawList>(0x4688e0)();at<uint8_t*>(0x6185e0)=head;
    if(!beginWorld()) return result;
    while(auto node=at<uint8_t*>(0x6185e0)) {
        drawNode(node);at<uint8_t*>(0x6185e0)=*reinterpret_cast<uint8_t**>(node);
    }
    LONGLONG t0=profileEnabled?profiler.now():0;
    if(gpuFrame) {
        collectGpuJobs();++batches;profiler.add(StageLegacy,t0);return result;
    }
    runRasterJobs();profiler.add(StageRaster,t0);t0=profileEnabled?profiler.now():0;
    ++batches;copyLegacyView();profiler.add(StageLegacy,t0);return result;
}

void fillUiSpan(int y,int left,int right,uint32_t color) {
    int x0=uiX[left],x1=uiX[right];
    for(int yy=uiY[y];yy<uiY[y+1];++yy)
        std::fill(rgba.begin()+size_t(yy)*width+x0,rgba.begin()+size_t(yy)*width+x1,color);
}
// UI layer. The game draws text and HUD into the 640x480 picture. With Direct3D the changed pixels are collected as
// a mask and drawn on top of the scaled scene at present time; the CPU renderer fills them straight into the frame.
uint8_t uiMask[480*640];uint32_t uiColorBuf[480*640];
bool uiRowDirty[480];int uiRowMinX[480],uiRowMaxX[480];int uiDirtyMin=480,uiDirtyMax=-1;
void uiSpan(int y,int left,int right,uint32_t color) {
    if(!(gpu.ready() && gpuRequested)){fillUiSpan(y,left,right,color);return;}
    for(int x=left;x<right;++x){uiMask[y*640+x]=1;uiColorBuf[y*640+x]=color;}
    if(!uiRowDirty[y]){uiRowDirty[y]=true;uiRowMinX[y]=left;uiRowMaxX[y]=right;}
    else{uiRowMinX[y]=std::min(uiRowMinX[y],left);uiRowMaxX[y]=std::max(uiRowMaxX[y],right);}
    uiDirtyMin=std::min(uiDirtyMin,y);uiDirtyMax=std::max(uiDirtyMax,y);
}
void clearUiMask() {
    for(int y=uiDirtyMin;y<=uiDirtyMax;++y)if(uiRowDirty[y]){std::memset(uiMask+y*640,0,640);uiRowDirty[y]=false;}
    uiDirtyMin=480;uiDirtyMax=-1;
}
void captionOverlay(const uint32_t* finalText) {
    for(int y=0;y<480;++y) {
        auto src=finalText+y*640;auto before=beforeText.data()+y*640;
        if(std::memcmp(src,before,640*4)==0)continue;
        for(int x=0;x<640;) {
            if(src[x]==before[x]) {++x;continue;}
            uint32_t c=src[x];int end=x+1;
            while(end<640 && src[end]==c && src[end]!=before[end])++end;
            uiSpan(y,x,end,c);x=end;
        }
    }
}
const uint32_t* convertColors=nullptr;
void convertRows(int top,int bottom) {
    size_t i=size_t(top)*width,count=size_t(bottom)*width;
    for(;i+4<=count;i+=4) {
        rgba[i]=convertColors[world[i]];rgba[i+1]=convertColors[world[i+1]];
        rgba[i+2]=convertColors[world[i+2]];rgba[i+3]=convertColors[world[i+3]];
    }
    for(;i<count;++i)rgba[i]=convertColors[world[i]];
}
void compose(const uint8_t* pixels,int pitch,const uint8_t* palette) {
    gameplayFramePresented=true;menuCacheValid=false;
    uint32_t colors[256];
    for(int i=0;i<256;++i)colors[i]=(uint32_t(palette[i*4])<<16)|(uint32_t(palette[i*4+1])<<8)|palette[i*4+2];
    for(int y=0;y<480;++y)for(int x=0;x<640;++x)beforeText[y*640+x]=colors[pixels[size_t(y)*pitch+x]];
    std::memcpy(lastColors,colors,sizeof colors);
    if(gpuFrame && gpuSceneReady) {
        gpu.setPalette(colors);
    } else {
    convertColors=colors;
    parallelRows(convertRows);
    }
    for(int y=0;y<480;++y) {
        auto src=pixels+size_t(y)*pitch;auto before=baseline.data()+y*640;
        if(std::memcmp(src,before,640)==0)continue;
        for(int x=0;x<640;) {
            if(src[x]==before[x]) {++x;continue;}
            uint8_t c=src[x];int end=x+1;
            while(end<640 && src[end]==c && src[end]!=before[end])++end;
            uiSpan(y,x,end,colors[c]);x=end;
        }
    }
    displayReady=true;
}
void __fastcall present8Hook(const uint8_t* pixels,int pitch,const uint8_t* palette) {
    displayReady=false;
    if(worldReady && pixels && palette && pitch>=640) {
        LONGLONG t0=profileEnabled?profiler.now():0;
        compose(pixels,pitch,palette);profiler.add(StageCompose,t0);++frames;
        if(frames==1) {char msg[180];std::snprintf(msg,sizeof msg,"first native frame: %dx%d, %llu scene primitives, %llu batches",width,height,primitives,batches);status(msg);}
    }
    fn<Present8>(0x46e630)(pixels,pitch,palette);
    if(profileEnabled){startWatchdog();InterlockedExchange64(&watchdogLast,profiler.now());}
}

// Capture artwork before text. The complete original menu stays fitted in the
// centre; extra space reflects actual artwork, with gradually softened detail.
uint16_t* __fastcall menuDecodeHook(const char* name) {
    // Screens such as LOADING are two JPEGs: IMAGES\POSTCARD.RRI (243x480 left strip, the base) and
    // IMAGES\POSTCARD\<route>.RRI (397x480 right part). The decoder copies the cached base picture into a
    // new slot and decodes the right part over it; if the base slot has been recycled the left 38 % stays
    // black. Decoding the base first makes sure it is resident (it is a cheap cache hit normally).
    if(name && fixPostcards) {
        if(std::strncmp(name,"Postcard/",9)==0)fn<MenuDecode>(0x435980)("Postcard");
        else if(std::strncmp(name,"Street/",7)==0)fn<MenuDecode>(0x435980)("Street");
    }
    auto raw=fn<MenuDecode>(0x435980)(name);
    if(profileEnabled && raw && name && std::strncmp(name,"Postcard/",9)==0) {
        size_t lit=0;for(int y=0;y<480;++y)for(int x=0;x<243;++x)lit+=raw[y*640+x]!=0;
        char m[200];std::snprintf(m,sizeof m,"menu image %s: left strip non-black pixels %zu of %d",name,lit,243*480);status(m);
    }
    if(raw) {
        menuBackdropDirty=true;
        menuBright.resize(640*480);menuDim.resize(640*480);
        auto convert=fn<const uint16_t*>(0x5b0170);
        for(size_t i=0;i<menuBright.size();++i){menuBright[i]=convert[raw[i]];menuDim[i]=convert[(raw[i]>>1)&0x3def];}
    }
    return raw;
}
uint32_t rgb16(uint16_t p,int r,int g,int b) {
    auto channel=[p](int mask) {if(!mask)return 0u;int shift=0;while(!(mask&1)){mask>>=1;++shift;}return ((p>>shift)&mask)*255u/mask;};
    return (channel(r)<<16)|(channel(g)<<8)|channel(b);
}
void prepare16(int r,int g,int b) {
    if(colorMasks[0]==r && colorMasks[1]==g && colorMasks[2]==b)return;
    colorMasks[0]=r;colorMasks[1]=g;colorMasks[2]=b;menuBackdropDirty=true;
    for(int i=0;i<65536;++i)color16[i]=rgb16(uint16_t(i),r,g,b);
}
uint32_t mixColor(uint32_t a,uint32_t b,unsigned weight) {
    unsigned inverse=256-weight;
    return ((((a&0xff00ff)*inverse+(b&0xff00ff)*weight)>>8)&0xff00ff)|
           ((((a&0x00ff00)*inverse+(b&0x00ff00)*weight)>>8)&0x00ff00);
}
void prepareMenuBackdrop(bool dim) {
    if(!menuBackdropDirty && menuBackdropDim==dim && menuBackdrop.size()==rgba.size())return;
    LONGLONG rebuildStart=profileEnabled?profiler.now():0;
    const auto& source=dim?menuDim:menuBright;
    std::array<std::vector<uint32_t>,4> softened;
    for(auto& level:softened)level.resize(640*480);
    std::vector<std::array<unsigned,3>> integral(641*481);
    for(int y=0;y<480;++y) {
        std::array<unsigned,3> sum{};
        for(int x=0;x<640;++x) {
            uint32_t c=color16[source[y*640+x]];softened[0][y*640+x]=c;
            for(int channel=0;channel<3;++channel) {
                sum[channel]+=(c>>(channel*8))&255;
                integral[(y+1)*641+x+1][channel]=integral[y*641+x+1][channel]+sum[channel];
            }
        }
    }
    const int radii[]={0,2,5,10};
    for(int level=1;level<4;++level)for(int y=0;y<480;++y)for(int x=0;x<640;++x) {
        int radius=radii[level],x0=std::max(0,x-radius),x1=std::min(640,x+radius+1);
        int y0=std::max(0,y-radius),y1=std::min(480,y+radius+1);
        unsigned count=(x1-x0)*(y1-y0);uint32_t color=0;
        for(int channel=0;channel<3;++channel) {
            unsigned sum=integral[y1*641+x1][channel]+integral[y0*641+x0][channel]
                        -integral[y0*641+x1][channel]-integral[y1*641+x0][channel];
            color|=(sum/count)<<(channel*8);
        }
        softened[level][y*640+x]=color;
    }
    // Reflect at the exact fitted artwork boundary, not a stretched edge strip.
    // Zero blur at the join avoids a rectangular brightness/detail discontinuity.
    // Coordinates repeat for unusually wide/tall windows without clipping art.
    struct Sample {int pixel;double distance;};
    auto samples=[](int count,int original,double scale,double offset) {
        std::vector<Sample> result(count);
        for(int i=0;i<count;++i) {
            double p=(i+0.5-offset)/scale;
            int pixel=wrap(int(std::floor(p)),original*2);
            result[i]={pixel<original?pixel:original*2-1-pixel,std::max({0.0,-p,p-original})};
        }
        return result;
    };
    double scale=std::min(width/640.0,height/480.0);
    auto xs=samples(width,640,scale,(width-640*scale)/2);
    auto ys=samples(height,480,scale,(height-480*scale)/2);
    menuBackdrop.assign(rgba.size(),0);
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
        if(mapX[x]>=0 && mapY[y]>=0)continue;
        double softness=std::min(3.0,std::max(xs[x].distance,ys[y].distance)/64.0);
        int level=std::min(2,int(softness));unsigned weight=unsigned((softness-level)*256);
        size_t src=ys[y].pixel*640+xs[x].pixel;
        menuBackdrop[size_t(y)*width+x]=mixColor(softened[level][src],softened[level+1][src],weight);
    }
    menuBackdropDim=dim;menuBackdropDirty=false;
    if(profileEnabled){LONGLONG took=profiler.now()-rebuildStart;profiler.stage[StageMenuBackdrop]+=took;++profiler.rebuilds;profiler.worstRebuild=std::max(profiler.worstRebuild,took);}
}
void menuSmoothRows(int top,int bottom) {
    int left=uiX[0],right=uiX[640];
    for(int y=std::max(top,uiY[0]);y<std::min(bottom,uiY[480]);++y) {
        const uint32_t* r0=beforeText.data()+size_t(smoothY0[y])*640;const uint32_t* r1=beforeText.data()+size_t(smoothY1[y])*640;
        unsigned wy=smoothWY[y];uint32_t* dst=rgba.data()+size_t(y)*width;
        for(int x=left;x<right;++x) {
            unsigned wx=smoothWX[x];int a=smoothX0[x],b=smoothX1[x];
            dst[x]=mixColor(mixColor(r0[a],r0[b],wx),mixColor(r1[a],r1[b],wx),wy);
        }
    }
}
void composeMenu(const void* pixels,int pitch,int r,int g,int b) {
    if(menuBright.size()!=640*480) return;
    LONGLONG menuStart=profileEnabled?profiler.now():0;
    // Match the original bright/dim background state without copying captions
    // or hover effects into the extension. Unchanged art dominates this sample.
    int brightMatches=0,dimMatches=0;
    for(int y=8;y<480;y+=16)for(int x=8;x<640;x+=16) {
        uint16_t p=reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(pixels)+size_t(y)*pitch)[x];
        brightMatches+=p==menuBright[y*640+x];dimMatches+=p==menuDim[y*640+x];
    }
    bool dim=dimMatches>brightMatches;
    prepare16(r,g,b);prepareMenuBackdrop(dim);
    for(int y=0;y<480;++y)for(int x=0;x<640;++x) {
        size_t i=y*640+x;uint16_t p=reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(pixels)+size_t(y)*pitch)[x];
        beforeText[i]=color16[p];
    }
    if(smoothUi && smoothWX.size()==size_t(width) && smoothWY.size()==size_t(height)) {
        // the menu picture rarely changes: rebuild the smoothed frame only when it (or the window) did
        uint64_t hash=1469598103934665603ull^(dim?0x9e3779b97f4a7c15ull:0);
        for(size_t i=0;i<beforeText.size();++i)hash=(hash^beforeText[i])*1099511628211ull;
        if(!menuCacheValid || hash!=menuCacheHash) {
            std::memcpy(rgba.data(),menuBackdrop.data(),rgba.size()*4);
            parallelRows(menuSmoothRows);
            menuCacheHash=hash;menuCacheValid=true;
        }
        displayReady=true;profiler.add(StageMenu,menuStart);return;
    }
    std::memcpy(rgba.data(),menuBackdrop.data(),rgba.size()*4);
    int previous=-1,left=uiX[0],right=uiX[640];
    for(int y=uiY[0];y<uiY[480];++y) {
        auto dst=rgba.data()+size_t(y)*width;
        if(mapY[y]==previous) {std::memcpy(dst+left,dst-width+left,(right-left)*4);continue;}
        previous=mapY[y];auto src=beforeText.data()+previous*640;
        for(int x=left;x<right;++x)dst[x]=src[mapX[x]];
    }
    displayReady=true;profiler.add(StageMenu,menuStart);
}
void __fastcall present16Hook(const void* pixels,int pitch,int r,int g,int b) {
    worldReady=false;displayReady=false;gpuSceneReady=false;
    // Menus are drawn with GDI: drop the race swap chain right away, or its last frame would stay frozen on screen.
    if(gpu.ready()){gpu.releaseWindow();InvalidateRect(at<HWND>(0x5ce858),nullptr,FALSE);}
    LONGLONG enter=profileEnabled?profiler.now():0;
    if(enabled && pixels && pitch>=1280 && at<int>(0x5ce85c)==640 && at<int>(0x5ce860)==480 && windowViewport())
        composeMenu(pixels,pitch,r,g,b);
    LONGLONG composed=profileEnabled?profiler.now():0;
    fn<Present16>(0x46e470)(pixels,pitch,r,g,b);
    if(profileEnabled)profiler.frame(enter,composed,profiler.now());
}
// ---- Direct3D present ---------------------------------------------------------------------
// Builds the quad list from this frame's jobs, uploads textures, draws backdrop + quads + UI on the
// GPU and presents. Returns false (and the caller falls back to GDI) if the device is unusable.
bool gpuBuildQuads(std::vector<rrhd::GpuQuad>& quads) {
    quads.clear();quads.reserve(frameJobs.size());
    fixSnapshotPointers(frameJobs);
    for(const auto& job:frameJobs) {
        rrhd::GpuQuad q{};
        for(int i=0;i<4;++i){q.x[i]=float(job.points[i].x);q.y[i]=float(job.points[i].y);q.u[i]=float(job.points[i].u);q.v[i]=float(job.points[i].v);}
        if(job.texture.pixels) {
            uint32_t xy=0,fl=0;
            if(!gpu.texture(job.texture.pixels,job.texture.width,job.texture.height,job.snapshot!=SIZE_MAX,xy,fl)) {
                if(gpu.staticAtlasFull())return false;
                else continue;
            }
            double area=0,umin=job.points[0].u,umax=umin,vmin=job.points[0].v,vmax=vmin;
            for(int i=0;i<4;++i){
                const auto& a=job.points[i];const auto& b=job.points[(i+1)&3];area+=a.x*b.y-b.x*a.y;
                umin=std::min(umin,a.u);umax=std::max(umax,a.u);vmin=std::min(vmin,a.v);vmax=std::max(vmax,a.v);
            }
            bool magnified=std::abs(area)*0.5>=(umax-umin)*(vmax-vmin)*1.4;
            q.atlasXY=xy;q.sizeWH=uint32_t(job.texture.width)|(uint32_t(job.texture.height)<<16);
            q.flags=fl|(job.texture.transparent?1u:0u)|((configFilter&&magnified&&!job.texture.transparent)?2u:0u)|((configFilter&&!job.texture.transparent)?16u:0u);   // sprites are never filtered
        } else {
            q.flags=4;q.color=job.color;
        }
        q.flags|=255u<<8;   // opacity 255 = solid (the shader lowers it for quads thinner than a pixel row)
        quads.push_back(q);
    }
    return true;
}
void gpuCompareWithCpu(const std::vector<uint32_t>& gpuPixels) {
    // Reference: the CPU path (indexed raster, nearest) for the same jobs, converted with the same palette.
    ensureCpuWorld();
    std::vector<uint8_t> savedWorld=world;
    paintBackdrop(legacyBackdrop.data(),640);
    for(const auto& job:frameJobs)
        rrhd::quad(world.data(),width,height,job.points,job.texture,job.color);
    uint64_t sum=0,diff=0,big=0;size_t firstX=0,firstY=0;bool seen=false;
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
        uint32_t a=lastColors[world[size_t(y)*width+x]],b=gpuPixels[size_t(y)*width+x];
        int d=std::abs(int((a>>16)&255)-int((b>>16)&255))+std::abs(int((a>>8)&255)-int((b>>8)&255))+std::abs(int(a&255)-int(b&255));
        sum+=d;if(d){++diff;if(d>48){++big;if(!seen){seen=true;firstX=x;firstY=y;}}}
    }
    world.swap(savedWorld);
    char m[260];std::snprintf(m,sizeof m,"gpu compare %dx%d: mean abs error %.3f per pixel, %.2f%% differ, %.3f%% differ strongly%s",
        width,height,double(sum)/(double(width)*height),100.0*diff/(double(width)*height),100.0*big/(double(width)*height),
        seen?(std::string(" (first strong diff at ")+std::to_string(firstX)+","+std::to_string(firstY)+")").c_str():"");
    status(m);
}
// Quality=Auto: watch how long a race frame takes to build and present. When the average stays above the budget
// the next lower render height is stored in RoadRashHD.ini ([Graphics] AutoRenderHeight) and used from the next start
// (changing the resolution in the middle of a race is not safe for the game).
bool autoQuality=false;
double autoFrameSum=0;int autoFrameCount=0,autoSlowWindows=0;bool autoDecided=false;
void autoQualityFrame(double ms) {
    if(!autoQuality || autoDecided)return;
    autoFrameSum+=ms;
    if(++autoFrameCount<600)return;                       // about 10 s of racing per window
    double mean=autoFrameSum/autoFrameCount;autoFrameSum=0;autoFrameCount=0;
    autoSlowWindows=mean>12.5?autoSlowWindows+1:0;       // budget: 12.5 of 16.7 ms
    if(autoSlowWindows<2)return;                          // two slow windows in a row
    static const int steps[]={1440,1080,900,720,540,480};
    int next=0;
    for(int s:steps)if(s<height){next=s;break;}
    autoDecided=true;
    if(next>0) {
        WritePrivateProfileStringW(L"Graphics",L"AutoRenderHeight",std::to_wstring(next).c_str(),configPath.c_str());
        char m[200];std::snprintf(m,sizeof m,"auto quality: %.1f ms per frame is over budget at %dp; next start will render at %dp",mean,height,next);status(m);
    }
}
bool gpuPresent() {
    // The swap chain exists only while a race is on screen. Menus, text screens and videos keep using the
    // GDI path so nothing else (Media Foundation, the game's own window changes) ever meets a flip-model chain.
    if(!(gpuSceneReady && gpuFrame)) {
        if(gpu.ready())gpu.releaseWindow();
        return false;
    }
    if(!ensureGpu())return false;
    gpu.beginFrame();
    LARGE_INTEGER autoT0{};if(autoQuality)QueryPerformanceCounter(&autoT0);
    rrhd::GpuFrameInfo info{};
    info.viewW=float(width);info.viewH=float(height);info.sceneScale=float(sceneScale);info.halfW=float(width/2.0);
    info.horizonW=horizonWidth;info.horizonRows=horizonRows;info.horizonScroll=horizonScroll;info.horizonY=horizonY;
    double ui=std::min(width/640.0,height/480.0);
    info.uiScale=float(ui);info.uiOx=float((width-640*ui)/2);info.uiOy=float((height-480*ui)/2);
    info.uiX0=float(uiX[0]);info.uiX1=float(uiX[640]);info.uiY0=float(uiY[0]);info.uiY1=float(uiY[480]);
    info.backdropFilter=configFilter?1.0f:0.0f;
    if(gpuHorizonDirty) {
        gpu.setHorizon(horizonRows>0?horizon.data():nullptr,horizonWidth,horizonRows);
        gpuHorizonDirty=false;
    }
    if(gpuSceneReady && gpuFrame) {
        static std::vector<rrhd::GpuQuad> quads;
        if(!gpuBuildQuads(quads)) {            // static atlas full: start over once
            gpu.flushTextures();gpu.clearStaticFull();
            if(!gpuBuildQuads(quads)){gpuSceneReady=false;return false;}
        }
        gpu.setLegacyBackdrop(legacyBackdrop.data());
        gpu.drawScene(info,quads.data(),quads.size());
        if(gpuCompare && gpu.readScene(gpuShot))gpuCompareWithCpu(gpuShot);
    } else {
        gpu.drawCpuFrame(rgba.data());
    }
    if(uiDirtyMax>=0) {
        static std::vector<uint32_t> layer(640*480);
        for(size_t i=0;i<layer.size();++i)layer[i]=uiMask[i]?(0xff000000u|uiColorBuf[i]):0u;
        gpu.drawUi(info,layer.data(),!smoothUi);   // flat (nearest) unless [Graphics] SmoothText=1
        clearUiMask();
    }
    gpu.boxX=boxX;gpu.boxY=boxY;gpu.boxW=boxW;gpu.boxH=boxH;
    bool ok=gpu.present(gpuVSync);
    if(autoQuality && autoT0.QuadPart) {
        LARGE_INTEGER t1,f;QueryPerformanceCounter(&t1);QueryPerformanceFrequency(&f);
        autoQualityFrame(1000.0*double(t1.QuadPart-autoT0.QuadPart)/double(f.QuadPart));
    }
    gpuSceneReady=false;
    return ok;
}

int WINAPI stretchHook(HDC dc,int x,int y,int w,int h,int sx,int sy,int sw,int sh,
                       const void* bits,const BITMAPINFO* bmi,UINT usage,DWORD rop) {
    if(displayReady && bits==at<void*>(0x5ce864) && sw==640 && sh==480 && !sx && !sy) {
        captionOverlay(static_cast<const uint32_t*>(bits));
        if(gpuRequested && !gpu.failed) {
            LONGLONG g0=profileEnabled?profiler.now():0;
            if(gpuPresent()) {
                if(profileEnabled){LONGLONG end=profiler.now();profiler.stage[StageGpu]+=end-g0;profiler.stage[StagePresent]+=end-g0;
                    profiler.presented(end);dumpFrame(end);}
                completedFrame=true;
                return height;
            }
        }
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        LONGLONG t0=profileEnabled?profiler.now():0;
        if(boxW!=clientWidth || boxH!=clientHeight) {   // black bars around the 4:3 picture
            if(boxX>0){PatBlt(dc,0,0,boxX,clientHeight,BLACKNESS);PatBlt(dc,boxX+boxW,0,clientWidth-boxX-boxW,clientHeight,BLACKNESS);}
            if(boxY>0){PatBlt(dc,0,0,clientWidth,boxY,BLACKNESS);PatBlt(dc,0,boxY+boxH,clientWidth,clientHeight-boxY-boxH,BLACKNESS);}
        }
        int rows=originalStretch(dc,boxX,boxY,boxW,boxH,0,0,width,height,rgba.data(),&info,DIB_RGB_COLORS,rop);
        if(profileEnabled){LONGLONG end=profiler.now();profiler.stage[StagePresent]+=end-t0;profiler.lastStretch=end-t0;
            profiler.maxStretch=std::max(profiler.maxStretch,end-t0);profiler.presented(end);dumpFrame(end);}
        if(rows>0)completedFrame=true;
        return rows;
    }
    // Pictures the game draws itself from its 640x480 buffer - the complete frame, or only a part of it
    // (the LOADING screen is assembled from a base picture and a 397x480 postcard) - are mapped through the
    // same fitted, centred, nearest-neighbour transform as every menu picture instead of using the game's
    // own destination rectangle, which assumed a different window shape.
    if(dc && bits && bmi && bmi->bmiHeader.biWidth==640 && std::abs(bmi->bmiHeader.biHeight)==480 &&
       sw>0 && sh>0 && sx>=0 && sy>=0 && sx+sw<=640 && sy+sh<=480) {
        HWND hwnd=at<HWND>(0x5ce858);RECT rc{};
        if(hwnd && GetClientRect(hwnd,&rc) && rc.right>0 && rc.bottom>0) {
            double s=std::min(rc.right/640.0,rc.bottom/480.0);
            int fw=int(640*s+0.5),fh=int(480*s+0.5),fx=(rc.right-fw)/2,fy=(rc.bottom-fh)/2;
            bool bottomUp=bmi->bmiHeader.biHeight>0;
            int topRow=bottomUp?480-(sy+sh):sy;                    // picture row of the top of the source rectangle
            int dx=fx+int(std::floor(sx*s)),dy=fy+int(std::floor(topRow*s));
            int dw=fx+int(std::floor((sx+sw)*s))-dx,dh=fy+int(std::floor((topRow+sh)*s))-dy;
            static int logged=0;
            if(profileEnabled && logged<24 && (sw!=640 || sh!=480)){
                char m[240];std::snprintf(m,sizeof m,"partial stretch: src %d,%d %dx%d -> game dest %d,%d %dx%d, ours %d,%d %dx%d (client %ldx%ld)",
                    sx,sy,sw,sh,x,y,w,h,dx,dy,dw,dh,rc.right,rc.bottom);status(m);++logged;
            }
            if(x!=dx || y!=dy || w!=dw || h!=dh) {
                if(sw==640 && sh==480) {                            // a full frame also repaints the bars around it
                    HBRUSH black=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
                    RECT bars[4]={{0,0,fx,rc.bottom},{fx+fw,0,rc.right,rc.bottom},{fx,0,fx+fw,fy},{fx,fy+fh,fx+fw,rc.bottom}};
                    for(const RECT& r:bars)if(r.right>r.left && r.bottom>r.top){if(originalFill)originalFill(dc,&r,black);else FillRect(dc,&r,black);}
                }
                int oldMode=SetStretchBltMode(dc,COLORONCOLOR);
                int rows=originalStretch(dc,dx,dy,dw,dh,sx,sy,sw,sh,bits,bmi,usage,rop);
                SetStretchBltMode(dc,oldMode);
                return rows;
            }
        }
    }
    // Any other GDI draw into the window while a stale race swap chain exists would be hidden by it.
    if(gpu.ready() && !(gpuFrame && frameActive)) {
        static DWORD lastRelease=0;DWORD now=GetTickCount();
        if(now-lastRelease>250){lastRelease=now;gpu.releaseWindow();}
    }
    return originalStretch(dc,x,y,w,h,sx,sy,sw,sh,bits,bmi,usage,rop);
}

int WINAPI fillHook(HDC dc,const RECT* rect,HBRUSH brush) {
    uintptr_t caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0))-base+0x400000;
    // Both original presentation routines clear their old letterbox directly
    // on the visible DC. Suppress exactly those clears, never HUD/control fills.
    if(displayReady && ((caller>=0x46ded0 && caller<0x46e074) ||
                        (caller>=0x46e630 && caller<0x46e920)))return 1;
    return originalFill(dc,rect,brush);
}

// Text encoding. The game reads its strings from TEXT\ENU.DLL with
// LoadStringA, which converts the stored UTF-16 text with the system ANSI code
// page. When Windows runs with "Use Unicode UTF-8 for worldwide language
// support" (ANSI code page 65001) every non-ASCII character (TM, curly quotes,
// dashes, accents) arrives as a multi-byte UTF-8 sequence, but the game's bitmap
// font is indexed by Windows-1252 code points, so it draws two or three garbage
// glyphs instead (for example "Rash" followed by A-circumflex, low double quote
// and cent sign). Strings that are fully representable in Windows-1252 are now
// returned in that code page, regardless of the system setting; anything else
// keeps the original behaviour.
using LoadStringFn=int(WINAPI*)(HINSTANCE,UINT,LPSTR,int);
LoadStringFn originalLoadString=nullptr;
int WINAPI loadStringHook(HINSTANCE instance,UINT id,LPSTR buffer,int bufferSize) {
    if(buffer && bufferSize>0) {
        static thread_local wchar_t wide[4100];
        int length=LoadStringW(instance,id,wide,int(sizeof wide/sizeof wide[0]));
        if(length>0) {
            BOOL lost=FALSE;
            int bytes=WideCharToMultiByte(1252,WC_NO_BEST_FIT_CHARS,wide,length,buffer,bufferSize-1,nullptr,&lost);
            if(bytes>0 && !lost){buffer[bytes]=0;return bytes;}
        }
    }
    return originalLoadString(instance,id,buffer,bufferSize);
}

// Diagnostics only ([Debug] KeyScript="seconds:vk[:holdMs],..."): the game reads
// the keyboard with GetAsyncKeyState/GetKeyboardState, so scripted key presses
// are injected there. This lets a headless test run reach a race without a
// foreground window. Disabled unless the ini key is present.
struct ScriptedKey {double at;int vk;double hold;};
std::vector<ScriptedKey> keyScript;
using AsyncKeyFn=SHORT(WINAPI*)(int);using KeyboardStateFn=BOOL(WINAPI*)(PBYTE);
AsyncKeyFn originalAsyncKey=nullptr;KeyboardStateFn originalKeyboardState=nullptr;
DWORD scriptStart=0;
bool scriptedDown(int vk) {
    double now=(GetTickCount()-scriptStart)/1000.0;
    for(const auto& k:keyScript)if(k.vk==vk && now>=k.at && now<k.at+k.hold)return true;
    return false;
}
SHORT WINAPI asyncKeyHook(int vk) {
    SHORT real=originalAsyncKey(vk);
    return scriptedDown(vk)?SHORT(real|0x8001):real;
}
BOOL WINAPI keyboardStateHook(PBYTE state) {
    BOOL ok=originalKeyboardState(state);
    if(ok&&state)for(const auto& k:keyScript)if(k.vk>0&&k.vk<256&&scriptedDown(k.vk))state[k.vk]|=0x80;
    return ok;
}
HWND WINAPI scriptedFocus() { return at<HWND>(0x5ce858); }
void installKeyScript(const wchar_t* text) {
    std::wstring s(text);size_t pos=0;
    while(pos<s.size()) {
        size_t end=s.find(L',',pos);if(end==std::wstring::npos)end=s.size();
        double at=0,hold=0.15;int vk=0;
        if(swscanf_s(s.substr(pos,end-pos).c_str(),L"%lf:%d:%lf",&at,&vk,&hold)>=2){if(hold>100)hold/=1000.0;keyScript.push_back({at,vk,hold});}
        pos=end+1;
    }
    if(keyScript.empty())return;
    scriptStart=GetTickCount();
    originalAsyncKey=at<AsyncKeyFn>(0x61c774);originalKeyboardState=at<KeyboardStateFn>(0x61c760);
    AsyncKeyFn a=&asyncKeyHook;KeyboardStateFn k=&keyboardStateHook;
    writeMemory(base+0x21c774,&a,sizeof a);writeMemory(base+0x21c760,&k,sizeof k);
    // A headless test window is never the focus window; make the game think it is.
    auto focusIat=at<void*>(0x61c71c);
    if(focusIat==reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetFocus"))) {
        HWND(WINAPI* f)()=&scriptedFocus;writeMemory(base+0x21c71c,&f,sizeof f);
    }
}

// Diagnostics (Profile=1): count calls of the per-object update (0x449bb0) so the effect of
// FullRateObjects can be measured without looking at the screen.
using ObjectUpdate=void(__fastcall*)(void*,int,char);
unsigned long long objectUpdates=0,objectTicksSeen=0;int lastObjectTick=-1;LONGLONG objectReportStart=0;
void __fastcall objectUpdateHook(void* object,int tick,char flag) {
    ++objectUpdates;
    if(tick!=lastObjectTick){lastObjectTick=tick;++objectTicksSeen;}
    LARGE_INTEGER now,freq;QueryPerformanceCounter(&now);QueryPerformanceFrequency(&freq);
    if(!objectReportStart)objectReportStart=now.QuadPart;
    if(now.QuadPart-objectReportStart>=freq.QuadPart*5) {
        char m[200];std::snprintf(m,sizeof m,"object updates: %llu in 5 s, %llu ticks, %.2f updates per tick",
            objectUpdates,objectTicksSeen,objectTicksSeen?double(objectUpdates)/objectTicksSeen:0.0);
        status(m);objectUpdates=0;objectTicksSeen=0;objectReportStart=now.QuadPart;
    }
    fn<ObjectUpdate>(0x449bb0)(object,tick,flag);
}

// Videos are played by Media Foundation into the game window (0x410b20, reached through the
// thunk at 0x403b2f). A flip-model swap chain owns the window surface, so it is released while
// a video plays and recreated by the next present.
using PlayVideo=void(__fastcall*)(const char*,int,int);
// Skipping a video with the controller. The game's video loop is a blocking GetMessage loop that looks at the
// pad only when some message arrives, so a button press went unnoticed. While a video plays, a helper thread
// watches the first XInput pad and posts a key press (Enter) to the game thread, which the loop treats as
// "skip" (the same as pressing a key). A, B, X, Y, Start or Back skip; a button already held when the video
// starts counts only after it has been released.
struct VideoSkipState{DWORD thread;volatile LONG done;};
DWORD WINAPI videoSkipThread(LPVOID context) {
    auto* state=static_cast<VideoSkipState*>(context);
    auto getState=xinputGetState();if(!getState)return 0;
    const WORD mask=0xf030;   // A B X Y Start Back
    WORD seenReleased=0,previous=mask;
    while(!state->done) {
        Sleep(30);
        XInputPad pad{};if(getState(0,&pad)!=0)continue;
        WORD down=WORD(pad.buttons&mask);
        seenReleased=WORD(seenReleased|(mask&~down));
        WORD fresh=WORD(down&~previous&seenReleased);
        previous=down;
        if(fresh)PostThreadMessageA(state->thread,WM_KEYDOWN,VK_RETURN,0);
    }
    return 0;
}
// Video volume ([Game] VideoVolume, 0-100). Both players (MCI for .avi, Media Foundation for .mp4) play through
// the process's default audio session, so while a video plays that session's volume is scaled by the setting and
// restored afterwards (the Windows volume mixer's own level for the game is kept as the reference).
struct VideoVolumeScope {
    ISimpleAudioVolume* volume=nullptr;float original=1.0f;bool uninitialize=false;
    explicit VideoVolumeScope(int percent) {
        if(percent>=100)return;
        HRESULT hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        uninitialize=SUCCEEDED(hr);
        IMMDeviceEnumerator* enumerator=nullptr;IMMDevice* device=nullptr;IAudioSessionManager* manager=nullptr;
        if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&enumerator))) &&
           SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender,eConsole,&device)) &&
           SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&manager))) &&
           SUCCEEDED(manager->GetSimpleAudioVolume(nullptr,FALSE,&volume)) && volume) {
            if(SUCCEEDED(volume->GetMasterVolume(&original)))volume->SetMasterVolume(original*(percent<0?0:percent)/100.0f,nullptr);
            else{volume->Release();volume=nullptr;}
        }
        if(manager)manager->Release();if(device)device->Release();if(enumerator)enumerator->Release();
    }
    ~VideoVolumeScope() {
        if(volume){volume->SetMasterVolume(original,nullptr);volume->Release();}
        if(uninitialize)CoUninitialize();
    }
};
void __fastcall playVideoHook(const char* name,int second,int argument) {
    if(gpu.ready()){gpu.releaseWindow();status("video: Direct3D swap chain released");}
    VideoSkipState skip{GetCurrentThreadId(),0};
    HANDLE thread=CreateThread(nullptr,0,videoSkipThread,&skip,0,nullptr);
    {
        VideoVolumeScope scope(videoVolume);
        fn<PlayVideo>(0x410b20)(name,second,argument);
    }
    InterlockedExchange(&skip.done,1);
    if(thread){WaitForSingleObject(thread,1000);CloseHandle(thread);}
}

// Window title: the game sets "Road Rash"; show the mod title with its version instead.
using SetWindowTextFn=BOOL(WINAPI*)(HWND,LPCSTR);
SetWindowTextFn originalSetWindowText=nullptr;
BOOL WINAPI setWindowTextHook(HWND window,LPCSTR text) {
    std::string title=text?text:"";
    if(title.compare(0,9,"Road Rash")==0)title=RRHD_WINDOW_TITLE;
    return originalSetWindowText(window,title.c_str());
}

// Skip the start-up videos. At the end of the start-up routine (0x460e80) the game
// plays "StudLogo", "PapyLogo" and "Intro" through three calls to the video player
// thunk (0x403b2f). With [Game] SkipIntro=1 those three calls do nothing; every
// other video (level, busted, wreck, ...) is untouched.
// The routine stops the audio thread before a video and (re)creates it afterwards (0x410660, CreateThread
// at 0x4036e3), so skipping the call outright left the game without music. Calling it with a video that does
// not exist takes the game's own "file not found" path: no video, but the audio thread is created.
void __fastcall skipIntroVideo(const char*,int second,int argument) {
    fn<PlayVideo>(0x410b20)("RRHD_NoSuchVideo",second,argument);
    if(profileEnabled){char m[120];std::snprintf(m,sizeof m,"intro video skipped (flags %d): audio thread handle %p",argument,at<void*>(0x61aff8));status(m);}
}

// Controller scan. The game's pad poll (0x463920) calls 0x4634a0 every two
// seconds while no pad is attached. That function opens every HID device on the
// system with CreateFileA looking for a Sony controller (VID 0x054c). A single
// slow device (Bluetooth, virtual or vendor HID) blocks the game thread for
// ~165 ms each time, which shows up as a periodic hitch in menus, credits and
// races. The scan now runs on a background thread; the game thread only
// learns about a pad on the next poll after the scan has succeeded.
volatile LONG hidScanRunning=0,hidScanFound=0;
DWORD WINAPI hidScanThread(LPVOID) {
    if(fn<uint8_t(__cdecl*)()>(0x4634a0)())InterlockedExchange(&hidScanFound,1);
    InterlockedExchange(&hidScanRunning,0);
    return 0;
}
// Controller answers message boxes. The game asks its questions (quit, home, ...) with plain Windows message boxes,
// which a controller cannot operate. While one is open, a helper thread watches the first XInput pad: the D-pad
// (or left stick) moves between the buttons, A presses the focused button, B answers "Cancel" / "No". A control
// already held when the box opens is ignored until it has been released, so the press that opened the box does
// not answer it.
// The message box is the visible, ENABLED dialog of the game thread: while it is open the dialog it belongs to
// (the game's own profile / save dialogs are #32770 windows of the same thread) is disabled.
BOOL CALLBACK findDialog(HWND window,LPARAM lp) {
    char name[16]{};GetClassNameA(window,name,sizeof name);
    if(std::strcmp(name,"#32770")==0 && IsWindowVisible(window) && IsWindowEnabled(window)){*reinterpret_cast<HWND*>(lp)=window;return FALSE;}
    return TRUE;
}
// Presses a button the way a mouse click does (it is drawn pressed).
void pressDialogButton(HWND dialog,int id) {
    HWND button=GetDlgItem(dialog,id);
    if(button)PostMessageA(button,BM_CLICK,0,0);else PostMessageA(dialog,WM_COMMAND,WPARAM(id),0);
}
DWORD WINAPI padDialogThread(LPVOID context) {
    auto* state=static_cast<PadDialogState*>(context);
    auto getState=xinputGetState();if(!getState)return 0;
    // D-pad / stick left-right moves the focus between the buttons, A presses the focused button, B cancels.
    // Every control counts only after it has been seen released since the box opened (the press that opened
    // the box must not answer it).
    const WORD mask=0x3000|0x000f;   // A, B, D-pad
    WORD seenReleased=0,previous=mask;
    while(!state->done) {
        Sleep(30);
        XInputPad pad{};if(getState(0,&pad)!=0)continue;
        WORD down=WORD(pad.buttons&mask);
        if(pad.lx<-16000)down|=0x0004;else if(pad.lx>16000)down|=0x0008;
        seenReleased=WORD(seenReleased|(mask&~down));
        WORD fresh=WORD(down&~previous&seenReleased);   // newly pressed since the last poll
        previous=down;
        if(!fresh)continue;
        HWND dialog=nullptr;EnumThreadWindows(state->thread,findDialog,reinterpret_cast<LPARAM>(&dialog));
        if(!dialog)continue;
        GUITHREADINFO info{};info.cbSize=sizeof info;
        char name[16]{};HWND focus=nullptr;
        if(GetGUIThreadInfo(state->thread,&info) && info.hwndFocus && IsChild(dialog,info.hwndFocus)){focus=info.hwndFocus;GetClassNameA(focus,name,sizeof name);}
        // Race Resolution dialog (list 1002, text 1001): the D-pad changes the selection directly, A applies, B cancels.
        if(GetDlgItem(dialog,1002) && GetDlgItem(dialog,1001)) {
            if(fresh&(0x0001|0x0002|0x0004|0x0008)) {
                HWND combo=GetDlgItem(dialog,1002);
                LRESULT count=SendMessageA(combo,CB_GETCOUNT,0,0),current=SendMessageA(combo,CB_GETCURSEL,0,0);
                if(count>0) {
                    bool previous=(fresh&(0x0001|0x0004))!=0;
                    LRESULT next=current<0?0:(previous?std::max<LRESULT>(0,current-1):std::min<LRESULT>(count-1,current+1));
                    SendMessageA(combo,CB_SETCURSEL,WPARAM(next),0);
                }
                continue;
            }
            if(fresh&0x1000){pressDialogButton(dialog,IDOK);continue;}
            if(fresh&0x2000){pressDialogButton(dialog,IDCANCEL);continue;}
        }
        // Open / Save as: D-pad up / down move through the file list (its ListBox is control 0x460, or the list view of an
        // Explorer-style dialog), whatever has the focus; A opens, B cancels (below).
        if(fresh&(0x0001|0x0002)) {
            HWND fileList=GetDlgItem(dialog,0x0460);
            if(!fileList){struct Find{static BOOL CALLBACK each(HWND w,LPARAM lp){char c[24]{};GetClassNameA(w,c,sizeof c);if(_stricmp(c,"SysListView32")==0){*reinterpret_cast<HWND*>(lp)=w;return FALSE;}return TRUE;}};
                EnumChildWindows(dialog,Find::each,reinterpret_cast<LPARAM>(&fileList));}
            if(fileList && IsWindowVisible(fileList)) {
                WPARAM key=(fresh&0x0001)?VK_UP:VK_DOWN;
                PostMessageA(fileList,WM_KEYDOWN,key,0);PostMessageA(fileList,WM_KEYUP,key,0);
                continue;
            }
        }
        bool pickList=focus && (_stricmp(name,"ComboBox")==0 || _stricmp(name,"ListBox")==0);   // up / down change the selection
        if(pickList && (fresh&(0x0001|0x0002))) {
            WPARAM key=(fresh&0x0001)?VK_UP:VK_DOWN;
            PostMessageA(focus,WM_KEYDOWN,key,0);PostMessageA(focus,WM_KEYUP,key,0);
        }
        else if(fresh&(0x0004|0x0001))PostMessageA(dialog,WM_NEXTDLGCTL,1,FALSE);   // previous control
        else if(fresh&(0x0008|0x0002))PostMessageA(dialog,WM_NEXTDLGCTL,0,FALSE);   // next control
        else if(fresh&0x1000) {
            // "The file already exists. Replace it?" of Save as: A answers Yes, whatever button has the focus (it starts on No).
            if(state->files && GetDlgItem(dialog,IDYES) && GetDlgItem(dialog,IDNO))pressDialogButton(dialog,IDYES);
            else if(focus && _stricmp(name,"Button")==0)PostMessageA(focus,BM_CLICK,0,0);
            else pressDialogButton(dialog,GetDlgItem(dialog,IDYES)?IDYES:IDOK);   // a list is focused: confirm the dialog
        } else if(fresh&0x2000)
            pressDialogButton(dialog,GetDlgItem(dialog,IDCANCEL)?IDCANCEL:GetDlgItem(dialog,IDNO)?IDNO:IDOK);
    }
    return 0;
}
using MessageBoxFn=int(WINAPI*)(HWND,LPCSTR,LPCSTR,UINT);
MessageBoxFn originalMessageBox=nullptr;
int WINAPI messageBoxHook(HWND owner,LPCSTR text,LPCSTR caption,UINT type) {
    StyleScope scope;
    if(!padDialogs)return originalMessageBox(owner,text,caption,type);
    PadDialogState state{GetCurrentThreadId(),0};
    InterlockedIncrement(&padBlock);
    HANDLE thread=CreateThread(nullptr,0,padDialogThread,&state,0,nullptr);
    int result=originalMessageBox(owner,text,caption,type);
    InterlockedExchange(&state.done,1);
    if(thread){WaitForSingleObject(thread,1000);CloseHandle(thread);}
    InterlockedDecrement(&padBlock);
    return result;
}

// Game language. 0x43f9f0 reads HKLM\SOFTWARE\Electronic Arts\RoadRash 95 "Language" (English, French, German,
// Italian or Spanish) and stores 0 / 0x10 / 0x20 / 0x30 / 0x40 (or -1) in 0x595968, which selects the strings in the
// executable and the Text\XXX.dll resource DLL. Writing HKLM needs administrator rights, so the language can be
// chosen in RoadRashHD.ini instead ([Game] Language=English|French|German|Italian|Spanish; Auto = registry).
char languageSetting[16]="Auto";
int languageCodeOf(const char* name) {
    static const char* const names[]={"English","French","German","Italian","Spanish"};
    for(int i=0;i<5;++i)if(_stricmp(name,names[i])==0)return i*0x10;
    return -1;
}
void __cdecl languageHook() {
    int forced=languageCodeOf(languageSetting);
    if(forced>=0){at<int>(0x595968)=forced;return;}
    // Same as the original: the registry decides.
    HKEY key=nullptr;char value[256]{};DWORD size=255;
    at<int>(0x595968)=-1;
    if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,"SOFTWARE\\Electronic Arts\\RoadRash 95",0,KEY_QUERY_VALUE,&key)!=0)return;
    if(RegQueryValueExA(key,"Language",nullptr,nullptr,reinterpret_cast<BYTE*>(value),&size)==0 && size<0x100)at<int>(0x595968)=languageCodeOf(value);
    RegCloseKey(key);
}

// Symmetric analog steering. The joystick layer turns the stick into "action" values in 0x46bea0 (ECX = action,
// EDX = how far the axis is from its end): for the left action (2) EDX is the raw axis (0..0x6fff) and for the
// right action (9) it is axis-0x7fff (0x1000..0x8000, the dead zone ends at 0x8fff). It stores EDX/800 in
// 0x603664 and the steering code (0x41bda0) uses  var-32  for left and  var  for right. Left therefore starts
// at +3 (slightly right) and reaches -32; right jumps to 5 and reaches 40, so the right turn is about twice as
// sensitive at half deflection, 25 % stronger at full deflection and has no soft start. Rewriting the mapping so
// that both sides run from 0 at the end of the dead zone to 32 at full deflection fixes that.
bool fixSteering=true;
int __fastcall steerAxisHook(int action,int amount) {
    if(amount<0)amount=0;
    if(action==0) {   // accelerate / brake axis: unchanged original behaviour
        int steerMax=*reinterpret_cast<int*>(at<uint8_t*>(0x5a69ec)+0x260);
        int value=int(int32_t(uint32_t(steerMax)*uint32_t(amount))/0x7fff);
        at<int>(0x603660)=value;return value;
    }
    if(action==2 || action==9) {
        double d=action==2?(0x6fff-double(amount))/0x6fff:(double(amount)-0x1000)/(0x8000-0x1000);
        d=std::max(0.0,std::min(1.0,d));
        at<int>(0x603664)=int(std::lround(32.0*(action==2?1.0-d:d)));
    }
    return 0;
}
uint8_t __cdecl hidScanHook() {
    if(hidScanFound)return 1; // The scan already filled in the game's pad state.
    if(InterlockedCompareExchange(&hidScanRunning,1,0)==0) {
        HANDLE thread=CreateThread(nullptr,0,hidScanThread,nullptr,0,nullptr);
        if(thread)CloseHandle(thread);else InterlockedExchange(&hidScanRunning,0);
    }
    return 0;
}

bool initialize(HMODULE module) {
    base=reinterpret_cast<uintptr_t>(module);
    struct Hook {uintptr_t thunk,original;void* hook;};
    Hook hooks[]={
        {0x401a2d,0x41be80,reinterpret_cast<void*>(&animationFrameHook)},
        {0x4058da,0x45ac50,reinterpret_cast<void*>(&frameHook)},
        {0x40132f,0x4688e0,reinterpret_cast<void*>(&listHook)},
        {0x4049a3,0x467b80,reinterpret_cast<void*>(&worldPresentHook)},
        {0x4017b7,0x468560,reinterpret_cast<void*>(&horizonLoadHook)},
        {0x4028ab,0x469770,reinterpret_cast<void*>(&horizonDrawHook)},
        {0x4050d8,0x435980,reinterpret_cast<void*>(&menuDecodeHook)},
        {0x40492b,0x46e630,reinterpret_cast<void*>(&present8Hook)},
        {0x401118,0x46e470,reinterpret_cast<void*>(&present16Hook)}
    };
    for(auto h:hooks)if(!isBranch(h.thunk,0xe9,h.original)){status("unsupported executable; no hooks installed");return false;}
    if(at<uint16_t>(0x45b3c0)!=0xc033 || at<uint16_t>(0x45b3c2)!=0x05c7 ||
       at<uintptr_t>(0x45b3c4)!=base+0x1cd224 || at<uintptr_t>(0x45b3c8)!=base+0x5bd20) {
        status("unsupported resolution menu; no hooks installed");return false;
    }
    originalStretch=at<Stretch>(0x61c15c);originalFill=at<Fill>(0x61c728);
    if(!originalStretch)return false;
    bool ok=true;for(auto h:hooks)ok=branch(h.thunk,h.hook,0xe9)&&ok;
    if(fixTextEncoding) {
        void* original=nullptr;
        if(hookImport(0x61c788,L"user32.dll","LoadStringA",reinterpret_cast<void*>(&loadStringHook),&original))originalLoadString=reinterpret_cast<LoadStringFn>(original);
        else status("text encoding fix skipped: unexpected import table");
    }
    {
        wchar_t script[1024]{};
        GetPrivateProfileStringW(L"Debug",L"KeyScript",L"",script,1024,configPath.c_str());
        if(script[0])installKeyScript(script);
    }
    // Full-rate object updates. The race scheduler (0x449890) updates every world
    // object through 0x449bb0, which ends by storing  nextUpdate = tick + interval
    // (0x449e81: mov eax,[esi+0x14]; add eax,ebx; ... mov [esi+0xc],eax). The interval
    // grows with the distance to the camera (1 tick nearby, up to 8 or more far away),
    // so distant riders, cars and pedestrians moved only every 2-8 ticks while the
    // snapshot for rendering was taken every tick. Replacing the load by "interval = 1"
    // makes every object update on every tick.
    if(fullRateObjects) {
        const uint8_t expect[5]={0x8b,0x46,0x14,0x03,0xc3};
        bool match=true;for(int i=0;i<5;++i)match=match && at<uint8_t>(0x449e81+i)==expect[i];
        if(match){const uint8_t patch[3]={0x31,0xc0,0x40};ok=writeMemory(base+0x49e81,patch,sizeof patch)&&ok;}
        else status("full-rate objects ignored: unexpected scheduler code");
    }
    if(profileEnabled) {
        for(uintptr_t thunk:{0x401d57,0x402b21})
            if(isBranch(thunk,0xe9,0x449bb0))ok=branch(thunk,reinterpret_cast<void*>(&objectUpdateHook),0xe9)&&ok;
    }
    {
        void* original=nullptr;
        if(hookImport(0x61c6dc,L"user32.dll","SetWindowTextA",reinterpret_cast<void*>(&setWindowTextHook),&original))originalSetWindowText=reinterpret_cast<SetWindowTextFn>(original);
    }
    if(isBranch(0x403b2f,0xe9,0x410b20))ok=branch(0x403b2f,reinterpret_cast<void*>(&playVideoHook),0xe9)&&ok;   // pad skip, and the swap chain release with Direct3D
    if(skipIntro) {
        const uintptr_t sites[]={0x461188,0x461196,0x4611a4};
        bool match=true;
        for(auto s:sites)match=match && at<uint8_t>(s)==0xe8 && uintptr_t(intptr_t(s+5)+at<int32_t>(s+1))==0x403b2f;
        if(match){for(auto s:sites)ok=branch(s,reinterpret_cast<void*>(&skipIntroVideo),0xe8)&&ok;}
        else status("skip intro ignored: unexpected start-up code");
    }
    if(keyboardAndPad) {
        // The game polls the keyboard and the pad in one routine (0x46bf40). Both loops walk the same list of
        // assignments (one key code and one pad code each); which of them runs depends on the control device saved in
        // the profile: a jump at 0x46bf5f skips the keyboard when the pad is selected, a jump at 0x46c028 skips the pad
        // when the keyboard is selected. Removing both jumps makes the keyboard and the pad work together (the pad part
        // stays harmless without a pad: its read fails and the routine returns). Each jump is verified with the compare
        // in front of it.
        // (The compares hold absolute addresses, which the loader rebases, so those are checked relative to the image base.)
        const uint8_t keyboardJump[6]={0x0f,0x85,0x8c,0x00,0x00,0x00},padJump[6]={0x0f,0x84,0x04,0x02,0x00,0x00};
        auto compareMatches=[&](uintptr_t va,uintptr_t variable)->bool {
            return at<uint8_t>(va)==0x80 && at<uint8_t>(va+1)==0x3d && at<uint32_t>(va+2)==uint32_t(base+variable-0x400000) && at<uint8_t>(va+6)==0x00;
        };
        bool match=compareMatches(0x46bf57,0x618480) && compareMatches(0x46c01b,0x5a43d8);
        for(int i=0;i<6;++i)match=match && at<uint8_t>(0x46bf5f+i)==keyboardJump[i] && at<uint8_t>(0x46c028+i)==padJump[i];
        if(match) {
            const uint8_t nops[6]={0x90,0x90,0x90,0x90,0x90,0x90};
            ok=writeMemory(base+0x46bf5f-0x400000,nops,6)&&ok;
            ok=writeMemory(base+0x46c028-0x400000,nops,6)&&ok;
            keyboardAndPadActive=true;
            status("input: keyboard and pad work together");
        }
        else {
            char message[200];
            std::snprintf(message,sizeof message,"keyboard and pad together ignored: unexpected input code (at 0x46bf57: %02x %02x %02x%02x%02x%02x %02x %02x %02x; at 0x46c01b: %02x %02x %02x%02x%02x%02x %02x %02x %02x)",
                at<uint8_t>(0x46bf57),at<uint8_t>(0x46bf58),at<uint8_t>(0x46bf59),at<uint8_t>(0x46bf5a),at<uint8_t>(0x46bf5b),at<uint8_t>(0x46bf5c),at<uint8_t>(0x46bf5d),at<uint8_t>(0x46bf5e),at<uint8_t>(0x46bf5f),
                at<uint8_t>(0x46c01b),at<uint8_t>(0x46c01c),at<uint8_t>(0x46c01d),at<uint8_t>(0x46c01e),at<uint8_t>(0x46c01f),at<uint8_t>(0x46c020),at<uint8_t>(0x46c021),at<uint8_t>(0x46c022),at<uint8_t>(0x46c023));
            status(message);
        }
    }
    if(fixBikeColors) {
        // The in-race bikes are drawn from one template and recoloured with one 256-byte palette remap per bike
        // (15 tables at 0x596a90, indexed by the bike's spec number: 0-4 Rat, 5-9 Super, 10-14 Sport). The Rat and
        // Super groups have their tables the wrong way round, so the 3D bike has the other class's colours (the
        // shop pictures are correct). Anakin94's fix for the 1996 exe swaps the same two groups of five tables.
        // The tables are verified by a hash first.
        const uintptr_t tables=0x596a90;const size_t size=256;
        uint64_t hash=0xcbf29ce484222325ull;
        for(size_t i=0;i<15*size;++i)hash=(hash^at<uint8_t>(tables+i))*0x100000001b3ull;
        if(hash==0x51d083b966bd8657ull) {
            const int pairs[5][2]={{0,5},{1,6},{2,7},{3,8},{4,9}};
            for(auto& p:pairs) {
                uint8_t a[size],b[size];
                std::memcpy(a,reinterpret_cast<void*>(base+tables-0x400000+p[0]*size),size);
                std::memcpy(b,reinterpret_cast<void*>(base+tables-0x400000+p[1]*size),size);
                ok=writeMemory(base+tables-0x400000+p[0]*size,b,size)&&ok;
                ok=writeMemory(base+tables-0x400000+p[1]*size,a,size)&&ok;
            }
        }
        else status("bike colour fix ignored: unexpected colour tables");
    }
    if(languageCodeOf(languageSetting)>=0) {
        const uint8_t expect[6]={0x55,0x8b,0xec,0x81,0xec,0x0c};
        bool match=true;for(int i=0;i<6;++i)match=match && at<uint8_t>(0x43f9f0+i)==expect[i];
        if(match)ok=branch(0x43f9f0,reinterpret_cast<void*>(&languageHook),0xe9)&&ok;
        else status("language setting ignored: unexpected start-up code");
    }
    {
        void* original=nullptr;
        if(hookImport(0x61c6f8,L"user32.dll","CreateWindowExA",reinterpret_cast<void*>(&createWindowHook),&original))originalCreateWindow=reinterpret_cast<CreateWindowFn>(original);
        if(autoHideCursor)hookImport(0x61c714,L"user32.dll","SetCursor",reinterpret_cast<void*>(&setCursorHook));
    }
    themeThread();
    {
        void* original=nullptr;   // window mode switch from the game menu, themed file dialogs
        if(hookImport(0x61c734,L"user32.dll","SetWindowPos",reinterpret_cast<void*>(&setWindowPosHook),&original))originalSetWindowPos=reinterpret_cast<SetWindowPosFn>(original);
        original=nullptr;
        if(hookImport(0x61c6e8,L"user32.dll","DispatchMessageA",reinterpret_cast<void*>(&dispatchMessageHook),&original))originalDispatchMessage=reinterpret_cast<DispatchMessageFn>(original);

    }

    if(padDialogs) {
        void* original=nullptr;
        if(hookImport(0x61c6a4,L"user32.dll","MessageBoxA",reinterpret_cast<void*>(&messageBoxHook),&original))originalMessageBox=reinterpret_cast<MessageBoxFn>(original);
        else status("controller dialogs ignored: unexpected import table");
    }
    if(fixSteering) {
        const uint8_t expect[8]={0x33,0xc0,0x85,0xd2,0x0f,0x48,0xd0,0x2b};
        bool match=true;for(int i=0;i<8;++i)match=match && at<uint8_t>(0x46bea0+i)==expect[i];
        if(match)ok=branch(0x46bea0,reinterpret_cast<void*>(&steerAxisHook),0xe9)&&ok;
        else status("symmetric steering ignored: unexpected input code");
    }
    // Optional: only when the call site matches exactly (and the user did not opt out).
    if(asyncControllerScan && at<uint8_t>(0x463961)==0xe8 &&
       uintptr_t(intptr_t(0x463961+5)+at<int32_t>(0x463962))==0x4634a0)
        ok=branch(0x463961,reinterpret_cast<void*>(&hidScanHook),0xe8)&&ok;
    Stretch hook=&stretchHook;ok=writeMemory(base+0x21c15c,&hook,sizeof hook)&&ok;
    Fill fill=&fillHook;ok=writeMemory(base+0x21c728,&fill,sizeof fill)&&ok;
    ok=branch(0x45b3c0,reinterpret_cast<void*>(&resolutionMenuHook),0xe9)&&ok;
    status(ok?(std::string(RRHD_NAME " v" RRHD_VERSION RRHD_BUILD_SUFFIX " (renderer v5): animation null-metadata fix, subpixel geometry, textured menu extension and native raster")).c_str():"hook installation failed");return ok;
}
}

// Read by the installer/launcher (it scans the file for this tag) to tell which MultiFix version is in the game folder.
extern "C" __declspec(dllexport) const char RRHD_VersionTag[]="RRHD_VERSION_TAG:" RRHD_VERSION;
extern "C" __declspec(dllexport) const char RRHD_BuildTag[]="RRHD_BUILD_TAG:" RRHD_BUILD_STAMP;

// [Performance] HighPerformance=1: ask Windows to treat the game as a foreground, full-speed workload.
//  * EcoQoS / execution-speed throttling off (hybrid laptops and Windows 11 otherwise may clock the process down),
//  * timer-resolution throttling off and a 1 ms system timer (accurate Sleep and frame limiter),
//  * slightly raised priority (ABOVE_NORMAL, never HIGH so the system stays responsive).
// The GPU's own power mode is a driver-wide setting (Intel Graphics Command Center) that no program can change.
static void applyPerformanceProfile() {
    struct PowerState{ULONG Version,ControlMask,StateMask;};
    HMODULE kernel=GetModuleHandleW(L"kernel32.dll");
    typedef BOOL(WINAPI* SetProcessInfoFn)(HANDLE,int,LPVOID,DWORD);
    auto setInfo=kernel?reinterpret_cast<SetProcessInfoFn>(GetProcAddress(kernel,"SetProcessInformation")):nullptr;
    bool throttleOff=false;
    if(setInfo) {
        PowerState s{1,0x1|0x4,0};                       // EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION, state 0 = never throttle
        throttleOff=setInfo(GetCurrentProcess(),4/*ProcessPowerThrottling*/,&s,sizeof s)!=FALSE;
        if(!throttleOff){s.ControlMask=0x1;throttleOff=setInfo(GetCurrentProcess(),4,&s,sizeof s)!=FALSE;} // Windows 10 before 11
    }
    bool priority=SetPriorityClass(GetCurrentProcess(),ABOVE_NORMAL_PRIORITY_CLASS)!=FALSE;
    bool timer=false;
    if(HMODULE mm=LoadLibraryW(L"winmm.dll")) {
        typedef UINT(WINAPI* BeginPeriodFn)(UINT);
        if(auto begin=reinterpret_cast<BeginPeriodFn>(GetProcAddress(mm,"timeBeginPeriod")))timer=begin(1)==0;
    }
    char m[200];std::snprintf(m,sizeof m,"performance profile: power throttling %s, 1 ms timer %s, priority above normal %s",
        throttleOff?"off":"unchanged",timer?"on":"unavailable",priority?"on":"unavailable");
    status(m);
}

extern "C" __declspec(dllexport) void __cdecl RRHD_Init(HMODULE module) {
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&RRHD_Init),&rendererModule);
    wchar_t path[32768]{};GetModuleFileNameW(module,path,32768);
    std::wstring directory(path);directory.resize(directory.find_last_of(L"\\/")+1);
    configPath=directory+L"RRWideScreen.ini";logPath=directory+L"RRWideScreen.log";
    enabled=GetPrivateProfileIntW(L"Graphics",L"Enabled",1,configPath.c_str())!=0;
    renderHeight=GetPrivateProfileIntW(L"Graphics",L"RenderHeight",0,configPath.c_str());
    if(renderHeight<=0) {   // Quality preset: used only while no fixed RenderHeight is set
        wchar_t q[24]{};GetPrivateProfileStringW(L"Graphics",L"Quality",L"High",q,24,configPath.c_str());
        if(!_wcsicmp(q,L"Medium"))renderHeight=720;
        else if(!_wcsicmp(q,L"Low"))renderHeight=540;
        else if(!_wcsicmp(q,L"Minimum"))renderHeight=480;
        else if(!_wcsicmp(q,L"Auto")) {
            autoQuality=true;
            renderHeight=GetPrivateProfileIntW(L"Graphics",L"AutoRenderHeight",0,configPath.c_str());
        }
        if(renderHeight>0){char m[120];std::snprintf(m,sizeof m,"quality preset: render height %d",renderHeight);status(m);}
    }
    // The Chinese-patch adapter only enables rendering and window features.
    gpu.lowLatency=GetPrivateProfileIntW(L"Performance",L"LowLatency",0,configPath.c_str())!=0;
    gpu.allowTearing=gpu.lowLatency;
    profileEnabled=GetPrivateProfileIntW(L"Graphics",L"Profile",0,configPath.c_str())!=0;
    asyncControllerScan=GetPrivateProfileIntW(L"Input",L"AsyncControllerScan",1,configPath.c_str())!=0;
    fixSteering=GetPrivateProfileIntW(L"Input",L"SymmetricSteering",1,configPath.c_str())!=0;
    padDialogs=GetPrivateProfileIntW(L"Input",L"PadDialogs",1,configPath.c_str())!=0;
    {wchar_t language[16]{};GetPrivateProfileStringW(L"Game",L"Language",L"Auto",language,16,configPath.c_str());
     for(int i=0;i<15;++i)languageSetting[i]=char(language[i]);languageSetting[15]=0;}
    fixTextEncoding=GetPrivateProfileIntW(L"Text",L"FixEncoding",1,configPath.c_str())!=0;
    gpuRequested=GetPrivateProfileIntW(L"Graphics",L"D3D11",1,configPath.c_str())!=0;
    configFilter=gpuRequested && GetPrivateProfileIntW(L"Graphics",L"TextureFilter",1,configPath.c_str())!=0;
    gpuVSync=GetPrivateProfileIntW(L"Graphics",L"VSync",0,configPath.c_str())!=0?1:0;
    smoothUi=GetPrivateProfileIntW(L"Graphics",L"SmoothText",0,configPath.c_str())!=0;
    gpuCompare=GetPrivateProfileIntW(L"Debug",L"CompareGpu",0,configPath.c_str())!=0;
    gpu.log=&status;
    gpu.timing=profileEnabled;
    {   // Wine / Proton: the swap chain goes on the game window itself (the child-window arrangement is a workaround
        // for the Windows DWM); [Graphics] ChildSurface=1/0 overrides the automatic choice.
        typedef const char*(__cdecl* WineVersion)();
        auto wineVersion=reinterpret_cast<WineVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"wine_get_version"));
        int surface=GetPrivateProfileIntW(L"Graphics",L"ChildSurface",-1,configPath.c_str());
        gpu.useChildWindow=surface>=0?surface!=0:wineVersion==nullptr;
        {   // environment line for bug reports
            typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);
            OSVERSIONINFOW os{};os.dwOSVersionInfoSize=sizeof os;
            auto rtlGetVersion=reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"RtlGetVersion"));
            if(rtlGetVersion)rtlGetVersion(&os);
            SYSTEM_INFO si{};GetNativeSystemInfo(&si);
            char m[200];std::snprintf(m,sizeof m,"environment: Windows %lu.%lu build %lu (reported), %lu logical CPUs, %s",os.dwMajorVersion,os.dwMinorVersion,os.dwBuildNumber,
                si.dwNumberOfProcessors,wineVersion?"Wine/Proton":"native Windows");
            status(m);
        }
        if(wineVersion){std::string m=std::string("running under Wine ")+wineVersion()+(gpu.useChildWindow?" (Direct3D surface: child window)":" (Direct3D surface: game window)");status(m.c_str());}
    }
    skipIntro=GetPrivateProfileIntW(L"Game",L"SkipIntro",0,configPath.c_str())!=0;
    fixPostcards=GetPrivateProfileIntW(L"Game",L"FixPostcards",1,configPath.c_str())!=0;
    fixBikeColors=GetPrivateProfileIntW(L"Game",L"FixBikeColors",1,configPath.c_str())!=0;
    keyboardAndPad=GetPrivateProfileIntW(L"Input",L"KeyboardAndPad",1,configPath.c_str())!=0;
    rememberLastSave=GetPrivateProfileIntW(L"Game",L"RememberLastSave",1,configPath.c_str())!=0;
    videoVolume=int(GetPrivateProfileIntW(L"Game",L"VideoVolume",100,configPath.c_str()));
    if(videoVolume<0)videoVolume=0;if(videoVolume>100)videoVolume=100;
    fullRateObjects=GetPrivateProfileIntW(L"Game",L"FullRateObjects",1,configPath.c_str())!=0;
    dumpEverySeconds=GetPrivateProfileIntW(L"Graphics",L"DumpFrameEverySeconds",0,configPath.c_str());
    dumpScale=GetPrivateProfileIntW(L"Graphics",L"DumpScale",2,configPath.c_str());
    windowAltEnter=GetPrivateProfileIntW(L"Window",L"AltEnterFullscreen",1,configPath.c_str())!=0;
    autoHideCursor=GetPrivateProfileIntW(L"Window",L"AutoHideCursor",1,configPath.c_str())!=0;
    startFullscreen=GetPrivateProfileIntW(L"Window",L"StartFullscreen",1,configPath.c_str())!=0;
    windowWidth=GetPrivateProfileIntW(L"Window",L"Width",0,configPath.c_str());
    windowHeight=GetPrivateProfileIntW(L"Window",L"Height",0,configPath.c_str());
    if(!GetPrivateProfileIntW(L"Window",L"WideWindow",1,configPath.c_str())){windowWidth=windowHeight=-1;wideGame=false;}
    // Rendering is independent of translated resources and of RRSaveSlots.dll.
    asyncControllerScan=false; fixSteering=false; padDialogs=false;
    fixTextEncoding=false; skipIntro=false; fullRateObjects=false;
    keyboardAndPad=false; fixBikeColors=false; fixPostcards=false;
    rememberLastSave=false; videoVolume=100;
    if(enabled)initialize(module);
}

// Zero-argument entry point used by the Chinese patch's independent module loader.
extern "C" __declspec(dllexport) void __cdecl RRWideInitialize() {
    static LONG initialized;
    if (!InterlockedCompareExchange(&initialized, 1, 0)) RRHD_Init(GetModuleHandleW(nullptr));
}
