#pragma once
#include "frontend_contract.hpp"
#include <thread>

namespace scripted::ui {

class Presenter {
public:
    Presenter(IView& v, Paths P)
    : view(v), P(std::move(P)) {
        cfg = ::scripted::loadConfig(this->P);
        wire();
        preloadAll(cfg, ws);
        pushBanks();
        view.showStatus("Ready. Loaded "+std::to_string(ws.banks.size())+" banks.");
    }

    // Expose for tests (optional)
    const Workspace& workspace() const { return ws; }
    const Config& config() const { return cfg; }

private:
    IView& view;
    Paths P;
    Config cfg;
    Workspace ws;
    std::optional<long long> current;
    bool dirty=false;
    std::atomic<bool> busy{false};

    void wire(){
        view.onPreload = [this](){ preload(); };
        view.onSwitch  = [this](const std::string& name){ openOrSwitch(name); };
        view.onSave    = [this](){ save(); };
        view.onResolve = [this](){ resolveAsync(); };
        view.onExport  = [this](){ exportAsync(); };
        view.onInsert  = [this](long long r,long long a,const std::string& v){ insert(r,a,v); };
        view.onDelete  = [this](long long r,long long a){ erase(r,a); };
        view.onFilter  = [this](const std::string& f){ filter = f; refreshRows(); };
    }

    std::string filter;

    void preload(){
        preloadAll(cfg, ws);
        pushBanks();
        view.showStatus("Preloaded "+std::to_string(ws.banks.size())+" banks.");
        refreshRows();
    }

    void pushBanks(){
        std::vector<std::pair<long long,std::string>> list;
        list.reserve(ws.banks.size());
        for (auto& [id,b] : ws.banks) list.emplace_back(id, b.title);
        view.showBankList(list);
        view.showCurrent(current);
    }

    void openOrSwitch(const std::string& nameOrStem){
        std::string status;
        if (!::scripted::openCtx(cfg, ws, nameOrStem, status)){
            view.showStatus(status);
            return;
        }
        std::string stem = nameOrStem;
        if (stem.size()>4 && stem.ends_with(".txt")) stem.resize(stem.size()-4);
        std::string token = (!stem.empty() && stem[0]==cfg.prefix)? stem.substr(1) : stem;
        long long id=0; parseIntBase(token, cfg.base, id);
        current = id; dirty=false;
        pushBanks();
        refreshRows();
        view.showStatus(status);
    }

    void refreshRows(){
        std::vector<Row> rows;
        if (current){
            auto& b = ws.banks[*current];
            for (auto& [rid, addrs] : b.regs)
                for (auto& [aid, val] : addrs)
                    rows.push_back({rid, aid, val});
            if (!filter.empty()){
                auto f = filter; std::transform(f.begin(), f.end(), f.begin(), ::tolower);
                std::vector<Row> out; out.reserve(rows.size());
                auto contains=[&](const std::string& s){
                    std::string h=s; std::transform(h.begin(), h.end(), h.begin(), ::tolower);
                    return h.find(f)!=std::string::npos;
                };
                for (auto& r: rows){
                    if (contains(toBaseN(r.reg,cfg.base,cfg.widthReg)) ||
                        contains(toBaseN(r.addr,cfg.base,cfg.widthAddr)) ||
                        contains(r.val)) out.push_back(r);
                }
                rows.swap(out);
            }
        }
        view.showRows(rows);
        view.showCurrent(current);
    }

    void insert(long long reg, long long addr, const std::string& val){
        if (!current){ view.showStatus("No current context"); return; }
        ws.banks[*current].regs[reg][addr] = val; dirty=true;
        refreshRows();
        view.showStatus("Updated "+toBaseN(reg,cfg.base,cfg.widthReg)+"."+toBaseN(addr,cfg.base,cfg.widthAddr));
    }

    void erase(long long reg, long long addr){
        if (!current){ view.showStatus("No current context"); return; }
        auto& regs = ws.banks[*current].regs;
        auto itR = regs.find(reg);
        if (itR!=regs.end()){
            if (itR->second.erase(addr)) { dirty=true; refreshRows(); view.showStatus("Deleted."); }
        }
    }

    void save(){
        if (!current){ view.showStatus("No current context"); return; }
        std::string err;
        auto path = contextFileName(cfg, *current);
        if (!saveContextFile(cfg, path, ws.banks[*current], err)){
            view.showStatus("Save failed: "+err);
            return;
        }
        dirty=false;
        view.showStatus("Saved "+path.string());
    }

    void resolveAsync(){
        if (!current){ view.showStatus("No current context"); return; }
        if (busy.exchange(true)){ view.showStatus("Busy..."); return; }
        view.setBusy(true);
        auto id=*current;
        std::thread([this,id](){
            std::string path; bool ok=true;
            try {
                auto txt = resolveBankToText(cfg, ws, id);
                auto outp = outResolvedName(cfg, id);
                std::ofstream out(outp, std::ios::binary); out<<txt;
                path = outp.string();
            } catch(...) { ok=false; }
            view.postToUi([this,ok,path](){
                view.setBusy(false);
                busy=false;
                view.showStatus(ok? "Resolved -> "+path : "Resolve failed.");
            });
        }).detach();
    }

    void exportAsync(){
        if (!current){ view.showStatus("No current context"); return; }
        if (busy.exchange(true)){ view.showStatus("Busy..."); return; }
        view.setBusy(true);
        auto id=*current;
        std::thread([this,id](){
            std::string path; bool ok=true;
            try {
                auto js = exportBankToJSON(cfg, ws, id);
                auto outp = outJsonName(cfg, id);
                std::ofstream out(outp, std::ios::binary); out<<js;
                path = outp.string();
            } catch(...) { ok=false; }
            view.postToUi([this,ok,path](){
                view.setBusy(false);
                busy=false;
                view.showStatus(ok? "Exported JSON -> "+path : "Export failed.");
            });
        }).detach();
    }
};

} // namespace scripted::ui
