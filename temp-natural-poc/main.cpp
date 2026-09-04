#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propsys.h>
#include <wrl/client.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
HWND g_main = nullptr;
HWND g_target = nullptr;
HWND g_shortcut = nullptr;
HWND g_log = nullptr;
std::wstring g_logPath;
bool g_busy = false;

constexpr int IDC_TARGET = 101;
constexpr int IDC_SHORTCUT = 102;
constexpr int IDC_PREPARE = 201;
constexpr int IDC_INSPECT = 202;
constexpr int IDC_SYSPIN = 203;
constexpr int IDC_IPINNED = 204;
constexpr int IDC_BOTH = 205;
constexpr int IDC_LAUNCH = 206;
constexpr int IDC_UNPIN = 207;
constexpr int IDC_CLEAR = 208;

std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

void AppendFile(const std::wstring& line) {
    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    auto b = Utf8(line + L"\r\n");
    DWORD w = 0;
    WriteFile(h, b.data(), (DWORD)b.size(), &w, nullptr);
    CloseHandle(h);
}

void Log(const std::wstring& msg) {
    SYSTEMTIME st{}; GetLocalTime(&st);
    wchar_t p[64]{};
    swprintf_s(p, L"[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::wstring line = p + msg;
    if (g_log) {
        int len = GetWindowTextLengthW(g_log);
        SendMessageW(g_log, EM_SETSEL, len, len);
        std::wstring x = line + L"\r\n";
        SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)x.c_str());
        SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
    }
    AppendFile(line);
}

std::wstring Text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize((size_t)n);
    return s;
}

void Busy(bool b) {
    g_busy = b;
    for (int id : {IDC_PREPARE,IDC_INSPECT,IDC_SYSPIN,IDC_IPINNED,IDC_BOTH,IDC_LAUNCH,IDC_UNPIN})
        EnableWindow(GetDlgItem(g_main,id), !b);
}

std::wstring ProgramsPath() {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &p)) || !p) return L"";
    std::wstring s(p); CoTaskMemFree(p); return s;
}

std::wstring DefaultTarget() {
    for (auto p : {L"C:\\Program Files\\Anki\\anki.exe",
                   L"C:\\Program Files\\Anki\\anki_launcher.exe",
                   L"C:\\Program Files (x86)\\Anki\\anki.exe",
                   L"C:\\Program Files (x86)\\Anki\\anki_launcher.exe"}) {
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) return p;
    }
    return L"C:\\Program Files\\Anki\\anki.exe";
}

std::wstring DefaultShortcut() {
    auto p = ProgramsPath();
    return p.empty() ? L"" : p + L"\\Anki Natural Identity POC.lnk";
}

bool EnsureParent(const std::wstring& file) {
    auto p = std::filesystem::path(file).parent_path();
    if (p.empty()) return true;
    int r = SHCreateDirectoryExW(nullptr, p.c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || std::filesystem::exists(p);
}

bool ReadShortcut(const std::wstring& path, std::wstring& target, std::wstring& aumid) {
    target.clear(); aumid.clear();
    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(hr)) return false;
    ComPtr<IPersistFile> pf;
    if (FAILED(link.As(&pf)) || FAILED(pf->Load(path.c_str(), STGM_READ))) return false;

    wchar_t buf[32768]{};
    WIN32_FIND_DATAW fd{};
    if (SUCCEEDED(link->GetPath(buf, (int)std::size(buf), &fd, SLGP_RAWPATH))) target = buf;

    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(link.As(&store))) {
        PROPVARIANT pv{}; PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &pv))) {
            if (pv.vt == VT_LPWSTR && pv.pwszVal) aumid = pv.pwszVal;
        }
        PropVariantClear(&pv);
    }
    return true;
}

bool CreateCleanShortcut() {
    const std::wstring target = Text(g_target);
    const std::wstring sc = Text(g_shortcut);
    if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(L"PREPARE: target missing: " + target); return false;
    }
    if (!EnsureParent(sc)) { Log(L"PREPARE: cannot create shortcut folder."); return false; }

    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(hr)) { Log(L"PREPARE: CoCreateInstance failed."); return false; }
    if (FAILED(link->SetPath(target.c_str()))) return false;
    auto wd = std::filesystem::path(target).parent_path().wstring();
    if (!wd.empty()) link->SetWorkingDirectory(wd.c_str());
    link->SetIconLocation(target.c_str(), 0);
    link->SetDescription(L"Anki natural Win32 identity test - intentionally no explicit AppUserModelID");

    ComPtr<IPersistFile> pf;
    if (FAILED(link.As(&pf)) || FAILED(pf->Save(sc.c_str(), TRUE))) {
        Log(L"PREPARE: Save failed."); return false;
    }

    std::wstring rt, ra;
    if (!ReadShortcut(sc, rt, ra)) { Log(L"PREPARE: saved but could not re-read shortcut."); return false; }
    Log(L"PREPARE: clean shortcut created.");
    Log(L"  target   = " + rt);
    Log(L"  shortcut = " + sc);
    Log(std::wstring(L"  explicit AUMID = ") + (ra.empty() ? L"<NONE> (expected)" : ra));
    if (!ra.empty()) {
        Log(L"PREPARE: FAIL - shortcut unexpectedly contains an explicit AUMID.");
        return false;
    }
    return true;
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM) {
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid=0; GetWindowThreadProcessId(hwnd,&pid);
    if (!pid) return TRUE;
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return TRUE;
    wchar_t image[32768]{}; DWORD n=(DWORD)std::size(image);
    bool isAnki=false;
    if (QueryFullProcessImageNameW(hp,0,image,&n)) {
        std::wstring name = std::filesystem::path(image).filename().wstring();
        for (auto& c : name) c=(wchar_t)towlower(c);
        isAnki = name == L"anki.exe" || name == L"anki_launcher.exe";
    }
    CloseHandle(hp);
    if (!isAnki) return TRUE;

    wchar_t title[1024]{}; GetWindowTextW(hwnd,title,(int)std::size(title));
    ComPtr<IPropertyStore> store;
    std::wstring aumid;
    if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store)))) {
        PROPVARIANT pv{}; PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID,&pv)) && pv.vt==VT_LPWSTR && pv.pwszVal) aumid=pv.pwszVal;
        PropVariantClear(&pv);
    }
    std::wstringstream ss;
    ss << L"RUNNING ANKI: HWND=0x" << std::hex << (UINT_PTR)hwnd << std::dec << L" PID=" << pid
       << L" title=\"" << title << L"\" explicit AUMID=" << (aumid.empty()?L"<NONE>":aumid);
    Log(ss.str());
    return TRUE;
}

void Inspect() {
    std::wstring t,a;
    const auto sc=Text(g_shortcut);
    Log(L"========== IDENTITY INSPECT ==========");
    if (ReadShortcut(sc,t,a)) {
        Log(L"SHORTCUT target = " + t);
        Log(std::wstring(L"SHORTCUT explicit AUMID = ") + (a.empty()?L"<NONE>":a));
    } else Log(L"SHORTCUT: not found/readable - click Prepare first.");
    EnumWindows(EnumWindowsProc,0);
    Log(L"NOTE: <NONE> on both shortcut and running Anki is the natural Win32 identity case we want to test.");
}

enum class PinnedListModifyCaller { Explorer = 4 };
constexpr GUID CLSID_TaskbandPin = {0x90aa3a4e,0x1cba,0x4233,{0xb8,0xbb,0x53,0x57,0x73,0xd4,0x84,0x49}};
class __declspec(uuid("0DD79AE2-D156-45D4-9EEB-3B549769E940")) IPinnedList3 : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE EnumObjects()=0;
    virtual HRESULT STDMETHODCALLTYPE GetPinnableInfo()=0;
    virtual HRESULT STDMETHODCALLTYPE IsPinnable()=0;
    virtual HRESULT STDMETHODCALLTYPE Resolve()=0;
    virtual HRESULT STDMETHODCALLTYPE LegacyModify()=0;
    virtual HRESULT STDMETHODCALLTYPE GetChangeCount()=0;
    virtual HRESULT STDMETHODCALLTYPE IsPinned(PCIDLIST_ABSOLUTE)=0;
    virtual HRESULT STDMETHODCALLTYPE GetPinnedItem()=0;
    virtual HRESULT STDMETHODCALLTYPE GetAppIDForPinnedItem()=0;
    virtual HRESULT STDMETHODCALLTYPE ItemChangeNotify()=0;
    virtual HRESULT STDMETHODCALLTYPE UpdateForRemovedItemsAsNecessary()=0;
    virtual HRESULT STDMETHODCALLTYPE PinShellLink()=0;
    virtual HRESULT STDMETHODCALLTYPE GetPinnedItemForAppID()=0;
    virtual HRESULT STDMETHODCALLTYPE Modify(PCIDLIST_ABSOLUTE,PCIDLIST_ABSOLUTE,PinnedListModifyCaller)=0;
};

bool GetList(ComPtr<IPinnedList3>& x) {
    return SUCCEEDED(CoCreateInstance(CLSID_TaskbandPin,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IPinnedList3),(void**)x.GetAddressOf()));
}

bool PinState(const std::wstring& sc, bool& pinned) {
    pinned=false; ComPtr<IPinnedList3> list; if(!GetList(list)) return false;
    PIDLIST_ABSOLUTE pidl=ILCreateFromPathW(sc.c_str()); if(!pidl) return false;
    HRESULT hr=list->IsPinned(pidl); ILFree(pidl);
    if(hr==S_OK){pinned=true;return true;} if(hr==S_FALSE){return true;} return false;
}

bool IPin(bool pin) {
    const auto sc=Text(g_shortcut);
    ComPtr<IPinnedList3> list; if(!GetList(list)){Log(L"IPINNED: COM unavailable.");return false;}
    PIDLIST_ABSOLUTE pidl=ILCreateFromPathW(sc.c_str()); if(!pidl){Log(L"IPINNED: ILCreateFromPath failed.");return false;}
    HRESULT hr = pin ? list->Modify(nullptr,pidl,PinnedListModifyCaller::Explorer)
                     : list->Modify(pidl,nullptr,PinnedListModifyCaller::Explorer);
    ILFree(pidl);
    std::wstringstream ss; ss<<L"IPINNED: Modify HRESULT=0x"<<std::hex<<std::uppercase<<(unsigned long)hr; Log(ss.str());
    if(FAILED(hr)) return false;
    for(int i=0;i<15;++i){Sleep(200);bool s=false;if(PinState(sc,s) && s==pin){Log(std::wstring(L"IPINNED verification = ")+(s?L"PINNED":L"not pinned"));return true;}}
    Log(L"IPINNED: HRESULT succeeded but verification did not match."); return false;
}

bool ExtractPttb(std::wstring& out) {
    HRSRC r=FindResourceW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(101),RT_RCDATA);
    if(!r){Log(L"PTTB: embedded resource missing.");return false;}
    HGLOBAL h=LoadResource(GetModuleHandleW(nullptr),r); DWORD sz=SizeofResource(GetModuleHandleW(nullptr),r);
    const void* p=h?LockResource(h):nullptr; if(!p||!sz)return false;
    wchar_t td[MAX_PATH]{}; if(!GetTempPathW(MAX_PATH,td))return false;
    auto dir=std::filesystem::path(td)/L"AnkiNaturalIdentityPOC"; std::error_code ec; std::filesystem::create_directories(dir,ec);
    auto f=dir/L"pttb.exe"; HANDLE hf=CreateFileW(f.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(hf==INVALID_HANDLE_VALUE)return false; DWORD w=0; BOOL ok=WriteFile(hf,p,sz,&w,nullptr); CloseHandle(hf);
    if(!ok||w!=sz)return false; out=f.wstring(); return true;
}

bool Pttb(bool unpin) {
    std::wstring exe; if(!ExtractPttb(exe))return false;
    auto sc=Text(g_shortcut);
    std::wstring cmd=L"\""+exe+L"\" "+(unpin?L"-u ":L"")+L"\""+sc+L"\"";
    std::vector<wchar_t> b(cmd.begin(),cmd.end());b.push_back(0);
    STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    BOOL ok=CreateProcessW(exe.c_str(),b.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi);
    if(!ok){Log(L"PTTB: CreateProcess failed: "+std::to_wstring(GetLastError()));return false;}
    WaitForSingleObject(pi.hProcess,15000);DWORD ec=999;GetExitCodeProcess(pi.hProcess,&ec);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    Log(L"PTTB: exit code = "+std::to_wstring(ec));Sleep(1200);
    bool s=false;if(PinState(sc,s)){Log(std::wstring(L"PTTB verification = ")+(s?L"PINNED":L"not pinned"));return unpin?!s:s;}
    return ec==0;
}

void LaunchAnki() {
    const auto target=Text(g_target);
    HINSTANCE r=ShellExecuteW(g_main,L"open",target.c_str(),nullptr,std::filesystem::path(target).parent_path().c_str(),SW_SHOWNORMAL);
    if((INT_PTR)r<=32) Log(L"LAUNCH: failed code="+std::to_wstring((INT_PTR)r));
    else Log(L"LAUNCH: Anki started. Check whether the running window activates the SAME pinned icon.");
}

void PrepareThen(const wchar_t* name, bool (*fn)()) {
    if(g_busy)return;Busy(true);
    Log(std::wstring(L"========== ")+name+L" ==========");
    if(CreateCleanShortcut()){Inspect();bool ok=fn();Log(std::wstring(L"RESULT = ")+(ok?L"SUCCESS":L"FAILED"));}
    Busy(false);
}

LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_COMMAND){
        switch(LOWORD(w)){
        case IDC_PREPARE: if(!g_busy){Busy(true);CreateCleanShortcut();Inspect();Busy(false);} return 0;
        case IDC_INSPECT: Inspect(); return 0;
        case IDC_SYSPIN: PrepareThen(L"SYSPIN/PTTB NO-AUMID", []()->bool{return Pttb(false);}); return 0;
        case IDC_IPINNED: PrepareThen(L"IPINNEDLIST3 NO-AUMID", []()->bool{return IPin(true);}); return 0;
        case IDC_BOTH:
            if(!g_busy){Busy(true);Log(L"========== TRY BOTH ==========");if(CreateCleanShortcut()){Inspect();bool ok=IPin(true);if(!ok){Log(L"TRY BOTH: IPinnedList3 failed; trying SysPin/PTTB.");ok=Pttb(false);}Log(std::wstring(L"TRY BOTH RESULT = ")+(ok?L"SUCCESS":L"FAILED"));}Busy(false);} return 0;
        case IDC_LAUNCH: LaunchAnki(); return 0;
        case IDC_UNPIN: if(!g_busy){Busy(true);Log(L"========== UNPIN TEST SHORTCUT ==========");if(!IPin(false)){Log(L"UNPIN: IPinnedList3 inconclusive; trying PTTB -u.");Pttb(true);}Busy(false);} return 0;
        case IDC_CLEAR: SetWindowTextW(g_log,L""); return 0;
        }
    }
    if(m==WM_DESTROY){PostQuitMessage(0);return 0;}
    return DefWindowProcW(h,m,w,l);
}

HWND Add(const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int hh,int id=0){
    return CreateWindowExW(0,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,hh,g_main,(HMENU)(INT_PTR)id,GetModuleHandleW(nullptr),nullptr);
}
}

int WINAPI wWinMain(HINSTANCE inst,HINSTANCE,LPWSTR,int show){
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    wchar_t tmp[MAX_PATH]{};GetTempPathW(MAX_PATH,tmp);g_logPath=std::wstring(tmp)+L"AnkiNaturalIdentityPOC.log";
    WNDCLASSW wc{};wc.lpfnWndProc=WndProc;wc.hInstance=inst;wc.lpszClassName=L"AnkiNaturalIdentityPOC";wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    RegisterClassW(&wc);
    g_main=CreateWindowExW(0,wc.lpszClassName,L"Anki Natural Taskbar Identity POC",WS_OVERLAPPEDWINDOW,120,100,980,700,nullptr,nullptr,inst,nullptr);
    HFONT f=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
    Add(L"STATIC",L"Target Anki executable:",0,15,15,170,22);
    g_target=Add(L"EDIT",DefaultTarget().c_str(),WS_BORDER|ES_AUTOHSCROLL,190,12,760,25,IDC_TARGET);
    Add(L"STATIC",L"Clean test shortcut (NO explicit AUMID):",0,15,50,250,22);
    g_shortcut=Add(L"EDIT",DefaultShortcut().c_str(),WS_BORDER|ES_AUTOHSCROLL,270,47,680,25,IDC_SHORTCUT);

    Add(L"BUTTON",L"1. Prepare clean shortcut",BS_PUSHBUTTON,15,85,190,32,IDC_PREPARE);
    Add(L"BUTTON",L"Inspect identity",BS_PUSHBUTTON,215,85,140,32,IDC_INSPECT);
    Add(L"BUTTON",L"SysPin/PTTB",BS_PUSHBUTTON,365,85,140,32,IDC_SYSPIN);
    Add(L"BUTTON",L"IPinnedList3",BS_PUSHBUTTON,515,85,140,32,IDC_IPINNED);
    Add(L"BUTTON",L"Try both",BS_PUSHBUTTON,665,85,120,32,IDC_BOTH);
    Add(L"BUTTON",L"Launch Anki",BS_PUSHBUTTON,795,85,155,32,IDC_LAUNCH);
    Add(L"BUTTON",L"Unpin test shortcut",BS_PUSHBUTTON,15,125,190,32,IDC_UNPIN);
    Add(L"BUTTON",L"Clear log",BS_PUSHBUTTON,215,125,140,32,IDC_CLEAR);
    Add(L"STATIC",L"Goal: pin an Anki shortcut with NO explicit AppUserModelID, then launch Anki and verify that Windows groups it under the same icon.",0,15,168,930,22);
    Add(L"STATIC",L"Important: manually unpin older POC/Anki pins first, otherwise visual verification can be misleading. This POC never edits your existing Anki shortcut.",0,15,193,930,22);
    g_log=Add(L"EDIT",L"",WS_BORDER|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL,15,225,935,420,0);
    for(HWND c=GetWindow(g_main,GW_CHILD);c;c=GetWindow(c,GW_HWNDNEXT))SendMessageW(c,WM_SETFONT,(WPARAM)f,TRUE);
    ShowWindow(g_main,show);UpdateWindow(g_main);
    Log(L"Anki Natural Identity POC started.");
    Log(L"This build intentionally does NOT call SetCurrentProcessExplicitAppUserModelID and does NOT write PKEY_AppUserModel_ID.");
    Log(L"Log file = "+g_logPath);
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}CoUninitialize();return 0;
}
