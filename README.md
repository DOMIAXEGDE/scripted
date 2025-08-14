# scripted
A Reference Resolver for Engineering Science and its Applications

Here’s the **updated, complete `script.cpp`** with:

* **Cross-context resolution** (lazy autoload from `files/`).
* **Both reference styles active at once**:

  * three-part numeric: `bank.register.address` (e.g. `2.1.7`)
  * two-part prefixed: `<prefix><bank>.<addr>` (e.g. `x00002.0007`, prefix/base/widths configurable)
* `@file(name.ext)` inclusion (reads from `files/`).
* `:preload` to eagerly load every `files/<prefix><id>.txt`.
* Clean `files/` I/O: `files/out/*.resolved.txt` and `files/out/*.json`.

### Build & run

```powershell
g++ -std=c++23 -O2 scripted.cpp -o scripted.exe
.\script.exe
```

```bash
g++ -std=c++23 -O2 scripted.cpp -o scripted
./script
```

### Quick commands

`:open x00001`, `:ins 0007 some text`, `:resolve`, `:export`, `:preload`, `:w`, `:ls`, `:show`, `:set prefix y`, `:set base 16`, `:set widths bank=5 addr=4 reg=2`, `:q`.

---

```cpp
// script.cpp
// C++23 line-editor + interpreter for a general-purpose data model with file context switching.
// Features:
// - Parses bank files like: x00001 (title : QUAL){  ... }
// - Resolves references across contexts, both styles active simultaneously:
//     * three-part numeric:   (\d+)\.(\d+)\.(\d+)         e.g., 2.1.7
//     * two-part prefixed:    ([A-Za-z])[0-9A-Za-z]+.[0-9A-Za-z]+  e.g., x00002.0007
//   (two-part uses configurable prefix/base/padding; resolves against register=1)
// - @file(name.ext) inlines files/<name.ext> during resolve.
// - Lazy autoload of referenced banks from files/; :preload to load all banks.
// - All I/O confined to files/ and files/out/.
// - Works on Windows PowerShell and Linux/macOS.

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <regex>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <limits>

namespace fs = std::filesystem;
using std::string;
using std::cout;
using std::endl;

// ----------------------------- Helpers -----------------------------
static inline string trim(string s) {
    auto notspace = [](int ch){ return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    return s;
}
static inline bool starts_with(const string& s, const string& p){
    return s.size()>=p.size() && std::equal(p.begin(), p.end(), s.begin());
}
static inline bool ends_with(const string& s, const string& p){
    return s.size()>=p.size() && std::equal(p.rbegin(), p.rend(), s.rbegin());
}
static int digitValue(char c){
    if (c>='0' && c<='9') return c-'0';
    if (c>='A' && c<='Z') return 10+(c-'A');
    if (c>='a' && c<='z') return 10+(c-'a');
    return -1;
}
static bool parseIntBase(const string& s, int base, long long& out){
    if (s.empty()) return false;
    long long v=0;
    for (char c: s){
        int d = digitValue(c);
        if (d<0 || d>=base) return false;
        v = v*base + d;
        // soft overflow guard (not strict)
        if (v < 0) return false;
    }
    out = v;
    return true;
}
static string toBaseN(long long val, int base, int width){
    if (base<2 || base>36) base=10;
    if (val==0){
        return string(std::max(1, width), '0');
    }
    bool neg = val<0; if (neg) val = -val;
    string s;
    while (val>0){
        int d = int(val%base);
        s.push_back(d<10? char('0'+d) : char('a'+(d-10)));
        val/=base;
    }
    if (neg) s.push_back('-');
    std::reverse(s.begin(), s.end());
    if (width>0 && (int)s.size()<width) s = string(width - (int)s.size(),'0') + s;
    return s;
}

// ----------------------------- Config -----------------------------
struct Config {
    char prefix = 'x';      // for two-part refs
    int base = 10;          // numeric base for bank/address tokens
    int widthBank = 5;
    int widthReg  = 2;
    int widthAddr = 4;

    string toJSON() const {
        std::ostringstream os;
        os << "{\n";
        os << "  \"prefix\": \"" << prefix << "\",\n";
        os << "  \"base\": " << base << ",\n";
        os << "  \"widthBank\": " << widthBank << ",\n";
        os << "  \"widthReg\": " << widthReg << ",\n";
        os << "  \"widthAddr\": " << widthAddr << "\n";
        os << "}\n";
        return os.str();
    }
    static Config fromJSON(const string& j){
        Config c;
        auto getStr=[&](const string& key, const string& def)->string{
            auto p = j.find("\""+key+"\"");
            if (p==string::npos) return def;
            p = j.find(':', p);
            if (p==string::npos) return def;
            p = j.find('"', p);
            if (p==string::npos) return def;
            auto q = j.find('"', p+1);
            if (q==string::npos) return def;
            return j.substr(p+1, q-(p+1));
        };
        auto getInt=[&](const string& key, int def)->int{
            auto p = j.find("\""+key+"\"");
            if (p==string::npos) return def;
            p = j.find(':', p);
            if (p==string::npos) return def;
            auto q = j.find_first_of(",\n}", p+1);
            string num = trim(j.substr(p+1, q-(p+1)));
            try { return std::stoi(num); } catch(...) { return def; }
        };
        string pf = getStr("prefix", "\"x\"");
        if (pf.size()>=2) c.prefix = pf[1]; // crude grab of the char between quotes
        c.base       = getInt("base", 10);
        c.widthBank  = getInt("widthBank", 5);
        c.widthReg   = getInt("widthReg", 2);
        c.widthAddr  = getInt("widthAddr", 4);
        return c;
    }
};

// ----------------------------- Model -----------------------------
struct Bank {
    long long id = 0;
    string title; // content inside parentheses
    // reg -> (addr -> value)
    std::map<long long, std::map<long long, string>> regs;

    bool empty() const {
        if (regs.empty()) return true;
        for (auto& [r, addrs] : regs) if (!addrs.empty()) return false;
        return true;
    }
};
struct Workspace {
    std::map<long long, Bank> banks;         // id -> Bank
    std::map<long long, string> filenames;   // id -> path
};

// ----------------------------- Paths -----------------------------
struct Paths {
    fs::path root      = "files";
    fs::path outdir    = root / "out";
    fs::path config    = root / "config.json";
    void ensure() const {
        fs::create_directories(root);
        fs::create_directories(outdir);
    }
} PATHS;

// ----------------------------- Parser / Writer -----------------------------
struct ParseResult { bool ok=true; string err; };

static ParseResult parseBankText(const string& text, const Config& cfg, Bank& outBank) {
    std::vector<string> lines;
    {
        std::istringstream is(text);
        string line;
        while (std::getline(is, line)) lines.push_back(line);
    }
    if (lines.empty()) return {false, "empty file"};

    // Header line(s)
    size_t i=0;
    while (i<lines.size() && trim(lines[i]).empty()) i++;
    if (i==lines.size()) return {false, "no header found"};
    string header = trim(lines[i]);

    // Accumulate until '{'
    string headerAccum = header;
    size_t j=i+1;
    while (headerAccum.find('{')==string::npos && j<lines.size()){
        headerAccum += " " + trim(lines[j]);
        j++;
    }
    if (headerAccum.find('{')==string::npos) return {false, "missing '{' after header"};

    // Extract ( ... )
    size_t lp = headerAccum.find('(');
    size_t rp = headerAccum.rfind(')');
    if (lp==string::npos || rp==string::npos || rp<lp) return {false, "malformed header: parentheses"};
    string left  = trim(headerAccum.substr(0, lp));      // e.g., x00001
    string title = trim(headerAccum.substr(lp+1, rp-lp-1));

    // Parse bank id from left (with or without prefix)
    if (!left.empty() && left[0]==cfg.prefix) left = left.substr(1);
    long long bankId;
    if (!parseIntBase(left, cfg.base, bankId)) return {false, "cannot parse bank id"};

    outBank = {};
    outBank.id = bankId;
    outBank.title = title;

    // Body starts after line that had '{'
    size_t bodyStartLine = i;
    while (bodyStartLine<lines.size() && lines[bodyStartLine].find('{')==string::npos) bodyStartLine++;
    if (bodyStartLine==lines.size()) return {false, "missing body start"};
    bodyStartLine++;

    long long currentReg = 1; // implicit default
    for (size_t k=bodyStartLine; k<lines.size(); ++k){
        string s = lines[k];
        if (s.find('}')!=string::npos) break;
        if (trim(s).empty()) continue;

        if (!s.empty() && s[0] != '\t'){ // register header (numeric in cfg.base)
            long long regId;
            if (!parseIntBase(trim(s), cfg.base, regId)){
                // Treat as stray line -> error for strictness
                return {false, "invalid register line: " + trim(s)};
            }
            currentReg = regId;
            continue;
        }
        // Address line: "\t<addr>\t<value>" (tab), but also tolerate space
        string t = s;
        while (!t.empty() && (t[0]=='\t' || t[0]==' ')) t.erase(t.begin());
        size_t sep = t.find('\t');
        if (sep==string::npos) sep = t.find(' ');
        string addrTok, val;
        if (sep==string::npos){ addrTok = trim(t); val=""; }
        else { addrTok = trim(t.substr(0, sep)); val = t.substr(sep+1); }

        long long addrId;
        if (!parseIntBase(addrTok, cfg.base, addrId))
            return {false, "invalid address id: " + addrTok};
        outBank.regs[currentReg][addrId] = val;
    }
    return {};
}

static string writeBankText(const Bank& b, const Config& cfg){
    std::ostringstream os;
    string bankStr = string(1,cfg.prefix) + toBaseN(b.id, cfg.base, cfg.widthBank);
    os << bankStr << "\t(" << b.title << "){\n";

    bool multi = (b.regs.size()>1) || (b.regs.size()==1 && b.regs.begin()->first!=1);
    if (!multi){
        auto it = b.regs.find(1);
        if (it != b.regs.end()){
            for (auto& [aid, val] : it->second){
                os << "\t" << toBaseN(aid, cfg.base, cfg.widthAddr) << "\t" << val << "\n";
            }
        }
    } else {
        for (auto& [rid, addrs] : b.regs){
            os << toBaseN(rid, cfg.base, cfg.widthReg) << "\n";
            for (auto& [aid, val] : addrs){
                os << "\t" << toBaseN(aid, cfg.base, cfg.widthAddr) << "\t" << val << "\n";
            }
        }
    }
    os << "}\n";
    return os.str();
}

// ----------------------------- Workspace IO -----------------------------
static fs::path contextFileName(const Config& cfg, long long bankId){
    return PATHS.root / (string(1,cfg.prefix) + toBaseN(bankId, cfg.base, cfg.widthBank) + ".txt");
}
static fs::path outResolvedName(const Config& cfg, long long bankId){
    return PATHS.outdir / (string(1,cfg.prefix) + toBaseN(bankId, cfg.base, cfg.widthBank) + ".resolved.txt");
}
static fs::path outJsonName(const Config& cfg, long long bankId){
    return PATHS.outdir / (string(1,cfg.prefix) + toBaseN(bankId, cfg.base, cfg.widthBank) + ".json");
}

static bool loadContextFile(const Config& cfg, const fs::path& file, Bank& bank, string& err){
    if (!fs::exists(file)) { err = "file not found: " + file.string(); return false; }
    std::ifstream in(file, std::ios::binary);
    if (!in){ err="cannot open: " + file.string(); return false; }
    string text( (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>() );
    ParseResult pr = parseBankText(text, cfg, bank);
    if (!pr.ok) { err = pr.err; return false; }
    return true;
}
static bool saveContextFile(const Config& cfg, const fs::path& file, const Bank& bank, string& err){
    std::ofstream out(file, std::ios::binary);
    if (!out){ err="cannot write: " + file.string(); return false; }
    out << writeBankText(bank, cfg);
    return true;
}

// --- ensure a bank exists in memory; if missing, autoload from files/ ---
static bool ensureBankLoadedInWorkspace(const Config& cfg, Workspace& ws, long long bankId, string& err){
    if (ws.banks.count(bankId)) return true;
    fs::path file = PATHS.root / (string(1,cfg.prefix) + toBaseN(bankId, cfg.base, cfg.widthBank) + ".txt");
    if (!fs::exists(file)) { err = "missing context file: " + file.string(); return false; }
    Bank b;
    if (!loadContextFile(cfg, file, b, err)) return false;
    ws.banks[bankId] = std::move(b);
    ws.filenames[bankId] = file.string();
    return true;
}

// ----------------------------- Resolver -----------------------------
// Applies, in order: @file(...), then three-part refs, then two-part refs.
// Recursion happens per match, so nested references are fully expanded.
// Cycle detection uses a visited set keyed by the concrete ref string.
struct Resolver {
    const Config& cfg;
    Workspace& ws; // mutable for autoload

    Resolver(const Config& c, Workspace& w): cfg(c), ws(w) {}

    bool getValue(long long bank, long long reg, long long addr, string& out) const {
        string err;
        (void)ensureBankLoadedInWorkspace(cfg, const_cast<Workspace&>(ws), bank, err);
        auto itB = ws.banks.find(bank);
        if (itB==ws.banks.end()) return false;
        const auto& b = itB->second;
        auto itR = b.regs.find(reg);
        if (itR==b.regs.end()) return false;
        auto itA = itR->second.find(addr);
        if (itA==itR->second.end()) return false;
        out = itA->second;
        return true;
    }
    bool getValueTwoPart(long long bank, long long addr, string& out) const {
        string err;
        (void)ensureBankLoadedInWorkspace(cfg, const_cast<Workspace&>(ws), bank, err);
        return getValue(bank, 1, addr, out); // default register 1
    }

    string includeFile(const string& name) const {
        fs::path p = PATHS.root / name; // relative to files/
        if (!fs::exists(p)) return string("[Missing file: ")+name+"]";
        std::ifstream in(p, std::ios::binary);
        if (!in) return string("[Cannot open file: ")+name+"]";
        return string( (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>() );
    }

    string resolve(const string& input, long long currentBank, std::unordered_set<string>& visited) const {
        string s = input;

        // 1) @file(...)
        {
            static std::regex fileRe(R"(@file\(([^)]+)\))");
            std::smatch m;
            string out; out.reserve(s.size());
            string::const_iterator searchStart( s.cbegin() );
            size_t last = 0;
            while (std::regex_search(searchStart, s.cend(), m, fileRe)) {
                size_t pos = m.position(0) + (searchStart - s.cbegin());
                size_t len = m.length(0);
                out.append(s, last, pos - last);
                string fname = trim(m[1].str());
                out += includeFile(fname);
                searchStart = s.cbegin() + pos + len;
                last = pos + len;
            }
            out.append(s, last, string::npos);
            s.swap(out);
        }

        // 2) three-part numeric: (\d+)\.(\d+)\.(\d+)
        {
            static std::regex tri(R"((\d+)\.(\d+)\.(\d+))");
            std::smatch m;
            string out; out.reserve(s.size());
            string::const_iterator searchStart( s.cbegin() );
            size_t last = 0;
            while (std::regex_search(searchStart, s.cend(), m, tri)) {
                size_t pos = m.position(0) + (searchStart - s.cbegin());
                size_t len = m.length(0);
                out.append(s, last, pos - last);

                long long b = std::stoll(m[1].str());
                long long r = std::stoll(m[2].str());
                long long a = std::stoll(m[3].str());
                string key = std::to_string(b)+"."+std::to_string(r)+"."+std::to_string(a);
                if (visited.count(key)) {
                    out += "[Circular Ref: " + m[0].str() + "]";
                } else {
                    string v;
                    if (!getValue(b, r, a, v)) {
                        out += "[Missing " + m[0].str() + "]";
                    } else {
                        auto visited2 = visited;
                        visited2.insert(key);
                        out += resolve(v, b, visited2);
                    }
                }

                searchStart = s.cbegin() + pos + len;
                last = pos + len;
            }
            out.append(s, last, string::npos);
            s.swap(out);
        }

        // 3) two-part prefixed: ([A-Za-z])([0-9A-Za-z]+)\.([0-9A-Za-z]+) — use cfg.prefix
        {
            static std::regex two(R"(([A-Za-z])([0-9A-Za-z]+)\.([0-9A-Za-z]+))");
            std::smatch m;
            string out; out.reserve(s.size());
            string::const_iterator searchStart( s.cbegin() );
            size_t last = 0;
            while (std::regex_search(searchStart, s.cend(), m, two)) {
                size_t pos = m.position(0) + (searchStart - s.cbegin());
                size_t len = m.length(0);
                out.append(s, last, pos - last);

                char pf = m[1].str()[0];
                if (pf != cfg.prefix) {
                    // leave other prefixes alone (could be text)
                    out += m[0].str();
                } else {
                    long long b=0, a=0;
                    if (!parseIntBase(m[2].str(), cfg.base, b) || !parseIntBase(m[3].str(), cfg.base, a)) {
                        out += "[BadRef " + m[0].str() + "]";
                    } else {
                        string key = string(1, pf) + m[2].str() + "." + m[3].str();
                        if (visited.count(key)) {
                            out += "[Circular Ref: " + m[0].str() + "]";
                        } else {
                            string v;
                            if (!getValueTwoPart(b, a, v)) {
                                out += "[Missing " + m[0].str() + "]";
                            } else {
                                auto visited2 = visited;
                                visited2.insert(key);
                                out += resolve(v, b, visited2);
                            }
                        }
                    }
                }

                searchStart = s.cbegin() + pos + len;
                last = pos + len;
            }
            out.append(s, last, string::npos);
            s.swap(out);
        }

        return s;
    }
};

// ----------------------------- Editor / REPL -----------------------------
struct Editor {
    Config cfg;
    Workspace ws;
    std::optional<long long> current; // current bank ID
    bool dirty=false;

    void loadConfig() {
        PATHS.ensure();
        if (fs::exists(PATHS.config)) {
            std::ifstream in(PATHS.config);
            string j( (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>() );
            cfg = Config::fromJSON(j);
        } else {
            std::ofstream out(PATHS.config);
            out << cfg.toJSON();
        }
    }
    void saveConfig() {
        std::ofstream out(PATHS.config);
        out << cfg.toJSON();
    }

    void help() {
        cout <<
R"(Commands:
  :help                          Show this help
  :open <ctx>                    Open/create context file (e.g., x00001)
  :switch <ctx>                  Switch to open context
  :preload                       Load all banks in files/ eagerly
  :ls                            List loaded contexts
  :show                          Print current buffer (header + addresses)
  :ins <addr> <value...>         Insert/replace address value in current context
  :del <addr>                    Delete an address
  :w                             Write current buffer to files/<ctx>.txt
  :r <path>                      Read/merge a raw model snippet from a file
  :resolve                       Write files/out/<ctx>.resolved.txt with refs expanded and @file() inlined
  :export                        Write files/out/<ctx>.json export
  :set prefix <char>             e.g., :set prefix y
  :set base <n>                  e.g., :set base 16
  :set widths bank=5 addr=4 reg=2
  :q                             Quit (prompts if dirty)
)"
        << endl;
    }

    bool openCtx(const string& ctxName) {
        string name = ctxName;
        if (ends_with(name, ".txt")) name = name.substr(0, name.size()-4);
        if (name.empty()) { cout<<"Bad name"<<endl; return false; }
        if (name[0]!=cfg.prefix) {
            cout << "Warning: context '"<<name<<"' doesn't start with prefix '"<<cfg.prefix<<"'. Proceeding.\n";
        }
        string token = (name[0]==cfg.prefix)? name.substr(1) : name;
        long long bankId;
        if (!parseIntBase(token, cfg.base, bankId)){
            cout<<"Cannot parse bank id from "<<name<<" with base "<<cfg.base<<"\n";
            return false;
        }
        fs::path file = contextFileName(cfg, bankId);
        Bank b;
        string err;
        if (fs::exists(file)) {
            if (!loadContextFile(cfg, file, b, err)){
                cout<<"Open failed: "<<err<<"\n";
                return false;
            }
        } else {
            b.id = bankId;
            b.title = name + " : NEW";
            b.regs[1] = {};
            string e;
            if (!saveContextFile(cfg, file, b, e)){
                cout<<"Create failed: "<<e<<"\n";
                return false;
            }
        }
        ws.banks[bankId] = std::move(b);
        ws.filenames[bankId] = file.string();
        current = bankId;
        dirty = false;
        cout<<"Opened "<<file<<endl;
        return true;
    }

    bool switchCtx(const string& ctxName){
        string name = ctxName;
        if (ends_with(name, ".txt")) name = name.substr(0, name.size()-4);
        string token = (name[0]==cfg.prefix)? name.substr(1) : name;
        long long bankId;
        if (!parseIntBase(token, cfg.base, bankId)){
            cout<<"Cannot parse bank id\n";
            return false;
        }
        if (!ws.banks.count(bankId)){
            return openCtx(ctxName);
        }
        current = bankId;
        cout<<"Switched to "<<ctxName<<"\n";
        return true;
    }

    void listCtx(){
        if (ws.banks.empty()) { cout<<"(no contexts loaded)\n"; return; }
        cout<<"Loaded contexts:\n";
        for (auto& [id,b] : ws.banks){
            cout<<"  "<<cfg.prefix<<toBaseN(id,cfg.base,cfg.widthBank)
                <<"  ("<<b.title<<")"
                <<(current && *current==id ? "   [current]" : "")
                <<"\n";
        }
    }

    bool ensureCurrent(){
        if (!current){ cout<<"No current context. Use :open <ctx>\n"; return false; }
        return true;
    }

    void show(){
        if (!ensureCurrent()) return;
        cout<<writeBankText(ws.banks[*current], cfg);
    }

    void write(){
        if (!ensureCurrent()) return;
        string err;
        if (!saveContextFile(cfg, contextFileName(cfg, *current), ws.banks[*current], err)){
            cout<<"Write failed: "<<err<<"\n";
        } else {
            cout<<"Saved "<<contextFileName(cfg, *current)<<"\n";
            dirty=false;
        }
    }

    void insert(const string& addrTok, const string& valueRaw){
        if (!ensureCurrent()) return;
        long long addr;
        if (!parseIntBase(addrTok, cfg.base, addr)) { cout<<"Bad address\n"; return; }
        ws.banks[*current].regs[1][addr] = valueRaw;
        dirty=true;
    }

    void del(const string& addrTok){
        if (!ensureCurrent()) return;
        long long addr;
        if (!parseIntBase(addrTok, cfg.base, addr)) { cout<<"Bad address\n"; return; }
        auto& m = ws.banks[*current].regs[1];
        size_t n = m.erase(addr);
        cout<<(n? "Deleted.\n" : "No such address.\n");
        dirty = dirty || (n>0);
    }

    void readMerge(const string& path){
        if (!ensureCurrent()) return;
        std::ifstream in(path, std::ios::binary);
        if (!in){ cout<<"Cannot open "<<path<<"\n"; return; }
        string text( (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>() );
        Bank tmp;
        auto pr = parseBankText(text, cfg, tmp);
        if (!pr.ok){ cout<<"Parse failed: "<<pr.err<<"\n"; return; }
        for (auto& [rid, addrs] : tmp.regs){
            for (auto& [aid, val] : addrs){
                ws.banks[*current].regs[rid][aid] = val;
            }
        }
        if (ws.banks[*current].title.empty()) ws.banks[*current].title = tmp.title;
        dirty=true;
        cout<<"Merged.\n";
    }

    void resolveOut(){
        if (!ensureCurrent()) return;
        Resolver R(cfg, ws);
        auto& b = ws.banks[*current];

        std::ostringstream os;
        string bankStr = string(1,cfg.prefix) + toBaseN(b.id, cfg.base, cfg.widthBank);
        os << bankStr << "\t(" << b.title << "){\n";
        for (auto& [rid, addrs] : b.regs){
            if (b.regs.size()>1) os << toBaseN(rid, cfg.base, cfg.widthReg) << "\n";
            for (auto& [aid, val] : addrs){
                std::unordered_set<string> visited;
                string out = R.resolve(val, b.id, visited);
                os << "\t" << toBaseN(aid, cfg.base, cfg.widthAddr) << "\t" << out << "\n";
            }
        }
        os << "}\n";
        fs::path outp = outResolvedName(cfg, b.id);
        std::ofstream out(outp, std::ios::binary);
        out << os.str();
        cout<<"Wrote "<< outp <<"\n";
    }

    void exportJson(){
        if (!ensureCurrent()) return;
        Resolver R(cfg, ws);
        auto& b = ws.banks[*current];
        std::ostringstream os;
        os << "{\n";
        os << "  \"bank\": \""<< cfg.prefix<<toBaseN(b.id,cfg.base,cfg.widthBank) <<"\",\n";
        os << "  \"title\": \""<< b.title <<"\",\n";
        os << "  \"registers\": [\n";
        bool firstR=true;
        for (auto& [rid, addrs] : b.regs){
            if (!firstR) os << ",\n";
            firstR=false;
            os << "    {\"id\":\""<<toBaseN(rid,cfg.base,cfg.widthReg)<<"\",\"addresses\":[\n";
            bool firstA=true;
            for (auto& [aid, val] : addrs){
                if (!firstA) os << ",\n";
                firstA=false;
                std::unordered_set<string> visited;
                string out = R.resolve(val, b.id, visited);
                auto esc = [](const string& s){
                    string r; r.reserve(s.size()*11/10 + 8);
                    for (char c: s){
                        if (c=='\\' || c=='"') { r.push_back('\\'); r.push_back(c); }
                        else if (c=='\n') { r += "\\n"; }
                        else r.push_back(c);
                    }
                    return r;
                };
                os << "      {\"id\":\""<<toBaseN(aid,cfg.base,cfg.widthAddr)
                   <<"\",\"value\":\""<<esc(out)<<"\"}";
            }
            os << "\n    ]}";
        }
        os << "\n  ]\n";
        os << "}\n";
        fs::path outp = outJsonName(cfg, b.id);
        std::ofstream out(outp, std::ios::binary);
        out << os.str();
        cout<<"Wrote "<< outp <<"\n";
    }

    void setOption(const std::vector<string>& toks){
        if (toks.size()<2){ cout<<":set what?\n"; return; }
        if (toks[1]=="prefix"){
            if (toks.size()<3 || toks[2].empty()) { cout<<"Usage: :set prefix <char>\n"; return; }
            cfg.prefix = toks[2][0];
            cout<<"prefix="<<cfg.prefix<<"\n";
            saveConfig();
        } else if (toks[1]=="base"){
            if (toks.size()<3){ cout<<"Usage: :set base <n>\n"; return; }
            int b = std::stoi(toks[2]);
            if (b<2 || b>36){ cout<<"base must be 2..36\n"; return; }
            cfg.base=b; saveConfig(); cout<<"base="<<cfg.base<<"\n";
        } else if (toks[1]=="widths"){
            for (size_t i=2;i<toks.size();++i){
                auto p = toks[i].find('=');
                if (p==string::npos) continue;
                string k=toks[i].substr(0,p), v=toks[i].substr(p+1);
                int n = std::stoi(v);
                if (k=="bank") cfg.widthBank=n;
                else if (k=="addr") cfg.widthAddr=n;
                else if (k=="reg") cfg.widthReg=n;
            }
            saveConfig();
            cout<<"widths bank="<<cfg.widthBank<<" reg="<<cfg.widthReg<<" addr="<<cfg.widthAddr<<"\n";
        } else {
            cout<<"Unknown :set option\n";
        }
    }

    void preloadAll(){
        PATHS.ensure();
        size_t loadedBefore = ws.banks.size();
        for (auto& entry : fs::directory_iterator(PATHS.root)){
            if (!entry.is_regular_file()) continue;
            auto p = entry.path();
            if (p.extension() != ".txt") continue;
            string stem = p.stem().string(); // e.g., x00001
            if (stem.empty() || stem[0]!=cfg.prefix) continue;
            long long id;
            if (!parseIntBase(stem.substr(1), cfg.base, id)) continue;
            string err;
            (void)ensureBankLoadedInWorkspace(cfg, ws, id, err);
        }
        cout<<"Preloaded contexts: "<< (ws.banks.size() - loadedBefore) <<" new, "<< ws.banks.size() <<" total.\n";
    }

    void repl(){
        loadConfig();
        cout<<"script.cpp interpreter (C++23) — files/* workspace\n";
        cout<<"Type :help for commands.\n\n";
        string line;
        while (true){
            cout<<">> ";
            if (!std::getline(std::cin, line)) break;
            string s = trim(line);
            if (s.empty()) continue;

            if (s==":help"){ help(); continue; }
            if (s==":ls"){ listCtx(); continue; }
            if (s==":show"){ show(); continue; }
            if (s==":w"){ write(); continue; }
            if (s==":preload"){ preloadAll(); continue; }
            if (s==":resolve"){ resolveOut(); continue; }
            if (s==":export"){ exportJson(); continue; }
            if (s==":q"){
                if (dirty){
                    cout<<"Unsaved changes. Type :w to save or :q again to quit.\n";
                    string l2; cout<<">> ";
                    if (!std::getline(std::cin,l2)) break;
                    if (trim(l2)==":q") break;
                    else { s = trim(l2); /* fallthrough to parse s */ }
                } else break;
            }

            // tokenized commands
            std::istringstream is(s);
            std::vector<string> tok;
            for (string t; is>>t;) tok.push_back(t);
            if (tok.empty()) continue;

            if (tok[0]==":open" && tok.size()>=2){
                openCtx(tok[1]); continue;
            }
            if (tok[0]==":switch" && tok.size()>=2){
                switchCtx(tok[1]); continue;
            }
            if (tok[0]==":ins" && tok.size()>=3){
                string value;
                for (size_t i=2;i<tok.size();++i){ if (i>2) value.push_back(' '); value+=tok[i]; }
                insert(tok[1], value); continue;
            }
            if (tok[0]==":del" && tok.size()>=2){
                del(tok[1]); continue;
            }
            if (tok[0]==":r" && tok.size()>=2){
                readMerge(tok[1]); continue;
            }
            if (tok[0]==":set"){
                setOption(tok); continue;
            }

            cout<<"Unknown command. :help\n";
        }
        cout<<"bye.\n";
    }
};

int main(){
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    PATHS.ensure();
    Editor ed;
    ed.repl();
    return 0;
}
```

The best description I can give — in terms of **scope of applications in engineering and the applied sciences** — is that your program is essentially a **general-purpose, reference-resolving, file-centric symbolic data environment**.

Here’s why:

---

### **High-level description**

This program is a **context-aware engineering knowledge editor** that:

* Organizes information into **modular “banks”** (contexts) of keyed entries.
* Allows these entries to **reference each other** — within the same bank or across banks — using formal numeric or prefixed address notation.
* Resolves these references **recursively** into complete, human-readable or machine-readable forms, with cycle detection.
* Embeds external source files or datasets directly into the resolution process.
* Stores everything in **plain text files** with a transparent and stable format.
* Produces **structured outputs** (`.txt` and `.json`) suitable for both human inspection and automated consumption.

---

### **Scope in engineering & applied sciences**

This tool could serve as the **core of a data-driven modeling and specification framework** in fields where:

1. **System design involves hierarchies or modular subsystems**

   * Electronics: FPGA module hierarchies, signal mapping tables, or testbench definitions.
   * Mechanical: component library IDs referencing geometry or materials specs.
2. **Large datasets require cross-referencing and reuse**

   * Aerospace: mission parameter libraries, with cross-links between subsystems.
   * Civil engineering: building standards, material properties, and code compliance tables.
3. **Formal knowledge representation & traceability are important**

   * Systems engineering: requirement IDs pointing to verification methods and test results.
   * Safety-critical domains: linking hazard IDs to mitigations and design evidence.
4. **Mixed-discipline integration**

   * Allowing mechanical, electrical, and software teams to share a common structured “index” of identifiers that resolve into full definitions.
5. **Applied science datasets**

   * Physics experiments: mapping calibration IDs to raw data files (`@file(...)`) and post-processing formulas.
   * Chemistry: cross-linked reaction schemes, lab protocols, and equipment parameter sets.

---

### **What makes it broadly applicable**

* **File-based, no hidden state** → Fits into any version-controlled workflow (Git, SVN, etc.).
* **Cross-context resolution** → Ideal for multi-disciplinary projects with shared identifiers.
* **Configurable numbering system** (base, prefix, padding) → Works with existing institutional or industry code formats.
* **Plain-text syntax + JSON export** → Enables both manual editing and machine parsing.
* **Recursive inlining** → Supports hierarchical definitions, parametric expansions, and modular design documentation.

---

If I were to distill it into one sentence:

> **A modular, file-based symbolic data system for defining, cross-referencing, and composing engineering and scientific information — enabling traceable, recursive resolution of complex, multi-context specifications into human-readable and machine-readable outputs.**
