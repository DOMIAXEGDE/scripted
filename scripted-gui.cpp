// scripted-gui.cpp — Win32 GUI using shared core.
// Build (MinGW):
// g++ -std=c++23 -O2 scripted-gui.cpp -o scripted-gui.exe -municode -lgdi32 -lcomctl32 -lcomdlg32 -lole32 -luuid

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <string>
#include <vector>

#include "scripted_core.hpp"

using namespace scripted;

// ---- UTF helpers ----
static std::wstring s2ws(const std::string& s){
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
static std::string ws2s(const std::wstring& w){
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::string trimS(std::string s){ return trim(std::move(s)); }

// ---- GUI IDs ----
enum : int {
    ID_BANK_COMBO = 1001,
    ID_BTN_SWITCH,
    ID_BTN_PRELOAD,
    ID_BTN_OPEN,
    ID_BTN_SAVE,
    ID_BTN_RESOLVE,
    ID_BTN_EXPORT,
    ID_LIST,
    ID_EDIT_VALUE,
    ID_EDIT_ADDR,
    ID_EDIT_REG,
    ID_BTN_INSERT,
    ID_BTN_DELETE,
    ID_STATUS
};

// ---- App state ----
struct App {
    Paths P;
    Config cfg;
    Workspace ws;
    std::optional<long long> current;
    bool dirty=false;

    HWND hwnd=nullptr,
         hCombo=nullptr,
         hBtnSwitch=nullptr,
         hBtnPreload=nullptr,
         hBtnOpen=nullptr,
         hBtnSave=nullptr,
         hBtnResolve=nullptr,
         hBtnExport=nullptr,
         hList=nullptr,
         hEditValue=nullptr,
         hEditAddr=nullptr,
         hEditReg=nullptr,
         hBtnInsert=nullptr,
         hBtnDelete=nullptr,
         hStatus=nullptr;

    void layout(){
        RECT rc; GetClientRect(hwnd, &rc);
        int W = rc.right - rc.left, H = rc.bottom - rc.top;
        int pad=8, row=32, btnW=90, btnH=28;

        // Row 1: Combo + Switch + actions
        MoveWindow(hCombo, pad, pad, 220, row, TRUE);
        int x = pad + 220 + 6;
        MoveWindow(hBtnSwitch,  x, pad, 80, btnH, TRUE); x += 80 + 6;
        MoveWindow(hBtnPreload, x, pad, btnW, btnH, TRUE); x += btnW + 4;
        MoveWindow(hBtnOpen,    x, pad, btnW, btnH, TRUE); x += btnW + 4;
        MoveWindow(hBtnSave,    x, pad, btnW, btnH, TRUE); x += btnW + 4;
        MoveWindow(hBtnResolve, x, pad, btnW, btnH, TRUE); x += btnW + 4;
        MoveWindow(hBtnExport,  x, pad, btnW, btnH, TRUE);

        int top2 = pad + row + pad;
        int listW = W/2 - (pad*1.5);
        int rightW = W - listW - pad*3;

        MoveWindow(hList, pad, top2, listW, H - top2 - (row+pad) - 4, TRUE);

        int rightX = pad*2 + listW;
        MoveWindow(hEditValue, rightX, top2, rightW, H - top2 - (row*2 + pad*2), TRUE);

        int bottomY = H - (row + pad);
        int editBoxW = 80;
        MoveWindow(hEditReg,  rightX, bottomY, 60, row, TRUE);
        MoveWindow(hEditAddr, rightX+60+6, bottomY, editBoxW, row, TRUE);
        MoveWindow(hBtnInsert,rightX+60+6+editBoxW+6, bottomY, 110, btnH, TRUE);
        MoveWindow(hBtnDelete,rightX+60+6+editBoxW+6+110+6, bottomY, 90, btnH, TRUE);

        MoveWindow(hStatus, pad, H-22, W - pad*2, 18, TRUE);
    }

    void setStatus(const std::string& s){ SetWindowTextW(hStatus, s2ws(s).c_str()); }

    void loadConfig(){ cfg = ::scripted::loadConfig(P); }
    void saveCfg(){ saveConfig(P, cfg); }

    void preloadAllUI(){
        preloadAll(cfg, ws);
        setStatus("Preloaded. Total banks: " + std::to_string(ws.banks.size()));
        refreshBankCombo();
    }

    void refreshBankCombo(){
        // Refill list items
        SendMessageW(hCombo, CB_RESETCONTENT, 0, 0);
        for (auto& [id, b] : ws.banks){
            std::wstring item = s2ws(string(1,cfg.prefix)+toBaseN(id,cfg.base,cfg.widthBank) + "  (" + b.title + ")");
            SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)item.c_str());
        }

        // Sync selection/text to current
        if (current){
            std::wstring curKey = s2ws(string(1,cfg.prefix)+toBaseN(*current,cfg.base,cfg.widthBank));
            int count = (int)SendMessageW(hCombo, CB_GETCOUNT, 0, 0);
            bool found=false;
            for(int i=0;i<count;++i){
                wchar_t buf[512]; SendMessageW(hCombo, CB_GETLBTEXT, i, (LPARAM)buf);
                std::wstring w(buf);
                if (w.rfind(curKey, 0)==0){ SendMessageW(hCombo, CB_SETCURSEL, i, 0); found=true; break; }
            }
            if (!found){
                // Not in the list yet; show the exact key in the edit box (editable combo)
                SetWindowTextW(hCombo, curKey.c_str());
            }
        }
    }

    bool openCtxUI(const std::string& nameOrStem){
        std::string status;
        if (!openCtx(cfg, ws, nameOrStem, status)){ setStatus(status); return false; }
        // Set current to the opened bank
        std::string stem = nameOrStem;
        if (stem.size()>4 && stem.substr(stem.size()-4)==".txt") stem = stem.substr(0, stem.size()-4);
        std::string token = (stem[0]==cfg.prefix)? stem.substr(1) : stem;
        long long id; parseIntBase(token, cfg.base, id);
        current = id; dirty=false;
        setStatus(status);
        refreshBankCombo();
        refreshList();
        return true;
    }

    void refreshList(){
        ListView_DeleteAllItems(hList);
        if (!current) return;
        auto& b = ws.banks[*current];
        int idx=0;
        for (auto& [rid, addrs] : b.regs){
            for (auto& [aid, val] : addrs){
                LVITEMW it{}; it.mask = LVIF_TEXT; it.iItem = idx;
                std::wstring regW = s2ws(toBaseN(rid, cfg.base, cfg.widthReg));
                std::wstring addrW= s2ws(toBaseN(aid, cfg.base, cfg.widthAddr));
                std::wstring valW = s2ws(val);
                it.pszText = regW.data();
                ListView_InsertItem(hList, &it);
                ListView_SetItemText(hList, idx, 1, addrW.data());
                ListView_SetItemText(hList, idx, 2, valW.data());
                idx++;
            }
        }
    }

    void saveCurrent(){
        if (!current){ setStatus("No current context"); return; }
        std::string err;
        if (!saveContextFile(cfg, contextFileName(cfg,*current), ws.banks[*current], err))
            setStatus("Write failed: "+err);
        else { dirty=false; setStatus("Saved "+contextFileName(cfg,*current).string()); }
    }

    void resolveCurrent(){
        if (!current){ setStatus("No current context"); return; }
        auto txt = resolveBankToText(cfg, ws, *current);
        auto outp = outResolvedName(cfg, *current);
        std::ofstream out(outp, std::ios::binary); out<<txt;
        setStatus("Resolved -> " + outp.string());
    }

    void exportJson(){
        if (!current){ setStatus("No current context"); return; }
        auto js = exportBankToJSON(cfg, ws, *current);
        auto outp = outJsonName(cfg, *current);
        std::ofstream out(outp, std::ios::binary); out<<js;
        setStatus("Exported JSON -> " + outp.string());
    }

    void selectRowToEditor(){
        int iSel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
        if (iSel<0) return;
        wchar_t regB[64]{}, addrB[64]{}, valB[65535]{};
        ListView_GetItemText(hList, iSel, 0, regB, 63);
        ListView_GetItemText(hList, iSel, 1, addrB, 63);
        ListView_GetItemText(hList, iSel, 2, valB, 65530);
        SetWindowTextW(hEditReg, regB);
        SetWindowTextW(hEditAddr, addrB);
        SetWindowTextW(hEditValue, valB);
    }

    void insertOrUpdateFromEditor(){
        if (!current){ setStatus("No current context"); return; }
        wchar_t regB[64]{}, addrB[64]{};
        int lenVal = GetWindowTextLengthW(hEditValue);
        std::wstring valW(lenVal, 0);
        GetWindowTextW(hEditReg, regB, 63);
        GetWindowTextW(hEditAddr, addrB, 63);
        GetWindowTextW(hEditValue, valW.data(), lenVal+1);
        std::string regS = ws2s(regB), addrS = ws2s(addrB), valS = ws2s(valW);
        if (trimS(addrS).empty()){ setStatus("Address required"); return; }
        long long regId=1, addrId=0;
        if (!trimS(regS).empty()){ if (!parseIntBase(trimS(regS), cfg.base, regId)){ setStatus("Bad reg"); return; } }
        if (!parseIntBase(trimS(addrS), cfg.base, addrId)){ setStatus("Bad addr"); return; }
        ws.banks[*current].regs[regId][addrId] = valS; dirty=true; refreshList();
        setStatus("Inserted/Updated " + toBaseN(regId,cfg.base,cfg.widthReg) + "." + toBaseN(addrId,cfg.base,cfg.widthAddr));
    }

    void deleteSelected(){
        if (!current) return;
        int iSel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
        if (iSel<0) return;
        wchar_t regB[64]{}, addrB[64]{};
        ListView_GetItemText(hList, iSel, 0, regB, 63);
        ListView_GetItemText(hList, iSel, 1, addrB, 63);
        std::string regS = ws2s(regB), addrS = ws2s(addrB);
        long long regId=1, addrId=0;
        if (!parseIntBase(trimS(regS), cfg.base, regId) || !parseIntBase(trimS(addrS), cfg.base, addrId)){ setStatus("Bad identifiers"); return; }
        auto& regs = ws.banks[*current].regs;
        auto itR = regs.find(regId);
        if (itR!=regs.end()){
            size_t n = itR->second.erase(addrId);
            if (n>0) { dirty=true; refreshList(); setStatus("Deleted"); }
        }
    }

    // New: switch (or open) based on the edit text in the combo.
    void switchFromCombo(){
        wchar_t wbuf[512]{};
        GetWindowTextW(hCombo, wbuf, 511);
        std::string entry = trimS(ws2s(wbuf));
        if (entry.empty()){
            setStatus("Enter a context (e.g., x00001)"); return;
        }
        // Normalize stem and id
        std::string stem = entry;
        if (stem.size()>4 && stem.substr(stem.size()-4)==".txt") stem = stem.substr(0, stem.size()-4);
        std::string token = (stem[0]==cfg.prefix)? stem.substr(1) : stem;
        long long id=0;
        if (!parseIntBase(token, cfg.base, id)){
            setStatus("Bad context id: " + entry);
            return;
        }
        if (ws.banks.count(id)){
            current = id;
            setStatus("Switched to " + stem);
            refreshBankCombo();
            refreshList();
        } else {
            // Not loaded yet — open it (will create if absent, consistent with CLI :open).
            openCtxUI(stem);
        }
    }
};

static App* gApp=nullptr;

static void CreateColumns(HWND hList){
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT|LVCF_WIDTH|LVCF_SUBITEM;
    col.pszText = (LPWSTR)L"Reg"; col.cx = 70; col.iSubItem=0; ListView_InsertColumn(hList, 0, &col);
    col.pszText = (LPWSTR)L"Addr"; col.cx = 80; col.iSubItem=1; ListView_InsertColumn(hList, 1, &col);
    col.pszText = (LPWSTR)L"Value (raw)"; col.cx = 600; col.iSubItem=2; ListView_InsertColumn(hList, 2, &col);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l){
    App& app = *gApp;
    switch (msg){
    case WM_CREATE:{
        InitCommonControls();
        // Make combo EDITABLE (CBS_DROPDOWN), not just a selection list.
        app.hCombo = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD|WS_VISIBLE|CBS_DROPDOWN,
                                     0,0,0,0, h, (HMENU)ID_BANK_COMBO, GetModuleHandleW(nullptr), nullptr);

        app.hBtnSwitch = CreateWindowExW(0, L"BUTTON", L"Switch", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                         0,0,0,0, h, (HMENU)ID_BTN_SWITCH, GetModuleHandleW(nullptr), nullptr);
        app.hBtnPreload = CreateWindowExW(0, L"BUTTON", L"Preload", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                          0,0,0,0, h, (HMENU)ID_BTN_PRELOAD, GetModuleHandleW(nullptr), nullptr);
        app.hBtnOpen = CreateWindowExW(0, L"BUTTON", L"Open/Reload", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                       0,0,0,0, h, (HMENU)ID_BTN_OPEN, GetModuleHandleW(nullptr), nullptr);
        app.hBtnSave = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                       0,0,0,0, h, (HMENU)ID_BTN_SAVE, GetModuleHandleW(nullptr), nullptr);
        app.hBtnResolve = CreateWindowExW(0, L"BUTTON", L"Resolve", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                          0,0,0,0, h, (HMENU)ID_BTN_RESOLVE, GetModuleHandleW(nullptr), nullptr);
        app.hBtnExport = CreateWindowExW(0, L"BUTTON", L"Export JSON", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                         0,0,0,0, h, (HMENU)ID_BTN_EXPORT, GetModuleHandleW(nullptr), nullptr);

        app.hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD|WS_VISIBLE|LVS_REPORT|LVS_SHOWSELALWAYS,
                                    0,0,0,0, h, (HMENU)ID_LIST, GetModuleHandleW(nullptr), nullptr);
        ListView_SetExtendedListViewStyle(app.hList, LVS_EX_FULLROWSELECT|LVS_EX_GRIDLINES|LVS_EX_DOUBLEBUFFER);
        CreateColumns(app.hList);

        app.hEditValue = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_LEFT|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,
                                         0,0,0,0, h, (HMENU)ID_EDIT_VALUE, GetModuleHandleW(nullptr), nullptr);

        app.hEditReg  = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"01", WS_CHILD|WS_VISIBLE|ES_LEFT|ES_AUTOHSCROLL,
                                         0,0,0,0, h, (HMENU)ID_EDIT_REG, GetModuleHandleW(nullptr), nullptr);
        app.hEditAddr = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD|WS_VISIBLE|ES_LEFT|ES_AUTOHSCROLL,
                                         0,0,0,0, h, (HMENU)ID_EDIT_ADDR, GetModuleHandleW(nullptr), nullptr);
        app.hBtnInsert= CreateWindowExW(0, L"BUTTON", L"Insert/Update", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                         0,0,0,0, h, (HMENU)ID_BTN_INSERT, GetModuleHandleW(nullptr), nullptr);
        app.hBtnDelete= CreateWindowExW(0, L"BUTTON", L"Delete", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                                         0,0,0,0, h, (HMENU)ID_BTN_DELETE, GetModuleHandleW(nullptr), nullptr);

        app.hStatus = CreateWindowExW(0, L"STATIC", L"Ready", WS_CHILD|WS_VISIBLE,
                                      0,0,0,0, h, (HMENU)ID_STATUS, GetModuleHandleW(nullptr), nullptr);

        app.P.ensure();
        app.loadConfig();
        app.layout();
        app.preloadAllUI();
        return 0;
    }
    case WM_SIZE:
        app.layout();
        return 0;
    case WM_NOTIFY:{
        LPNMHDR hdr = (LPNMHDR)l;
        if (hdr->idFrom == ID_LIST && hdr->code == LVN_ITEMCHANGED){
            LPNMLISTVIEW lv = (LPNMLISTVIEW)l;
            if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)){
                app.selectRowToEditor();
            }
        }
        return 0;
    }
    case WM_COMMAND:{
        int id = LOWORD(w);
        if (id == ID_BTN_SWITCH){
            app.switchFromCombo();
        }
        else if (id == ID_BTN_PRELOAD){
            app.preloadAllUI();
        }
        else if (id == ID_BTN_OPEN){
            OPENFILENAMEW ofn{}; wchar_t buf[1024]=L"";
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = h;
            ofn.lpstrFilter = L"Bank files (*.txt)\0*.txt\0All files\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.lpstrFile = buf; ofn.nMaxFile = 1024;
            ofn.lpstrInitialDir = s2ws(app.P.root.string()).c_str();
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)){
                std::wstring ws(buf); std::string path = ws2s(ws);
                scripted::fs::path p(path);
                app.openCtxUI(p.stem().string());
            }
        }
        else if (id == ID_BTN_SAVE){
            app.saveCurrent();
        }
        else if (id == ID_BTN_RESOLVE){
            app.resolveCurrent();
        }
        else if (id == ID_BTN_EXPORT){
            app.exportJson();
        }
        else if (id == ID_BTN_INSERT){
            app.insertOrUpdateFromEditor();
        }
        else if (id == ID_BTN_DELETE){
            app.deleteSelected();
        }
        else if (id == ID_BANK_COMBO){
            // If user picks from dropdown list, switch as before.
            if (HIWORD(w) == CBN_SELCHANGE){
                int idx = (int)SendMessageW(app.hCombo, CB_GETCURSEL, 0, 0);
                if (idx>=0){
                    wchar_t buf[512]; SendMessageW(app.hCombo, CB_GETLBTEXT, idx, (LPARAM)buf);
                    std::string line = ws2s(buf);
                    std::string name = line.substr(0, line.find(' '));
                    std::string token = (name[0]==app.cfg.prefix)? name.substr(1) : name;
                    long long idv=0;
                    if (parseIntBase(trim(std::move(token)), app.cfg.base, idv)){
                        app.current = idv;
                        app.refreshList();
                        app.setStatus("Switched to " + name);
                        // also reflect exact text in edit
                        SetWindowTextW(app.hCombo, s2ws(name).c_str());
                    }
                }
            }
            // Optional: pressing Enter in the combo edit triggers "Switch"
            if (HIWORD(w) == CBN_EDITCHANGE){
                // no-op; user can press the Switch button
            }
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int){
    static App app; gApp=&app;
    WNDCLASSW wc{}; wc.hInstance = hInst; wc.lpszClassName = L"ScriptedGuiWnd"; wc.lpfnWndProc = WndProc;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"scripted-gui — Bank Editor & Resolver",
                                WS_OVERLAPPEDWINDOW|WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 700,
                                nullptr, nullptr, hInst, nullptr);
    app.hwnd = hwnd;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)){ TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
