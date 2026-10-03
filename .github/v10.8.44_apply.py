from pathlib import Path


def replace_once(path, old, new, label):
    p = Path(path)
    s = p.read_text()
    count = s.count(old)
    if count != 1:
        raise SystemExit(f"{label}: matches={count}")
    p.write_text(s.replace(old, new, 1))


replace_once(
    "src/AppVersion.h",
    'inline constexpr const char* kProgramVersion = "10.8.43";',
    'inline constexpr const char* kProgramVersion = "10.8.44";',
    "version",
)

replace_once(
    "src/OscamMiniManager.h",
    '''    static std::string run(const std::string& command, int* rc = nullptr);\n    static std::vector<std::string> ttyDevices();\n    static std::string json(const Json::Value& value);\n''',
    '''    struct DetectedReader {\n        std::string kind;\n        std::string name;\n        std::string protocol;\n        std::string device;\n        std::string detail;\n        bool stable = false;\n    };\n\n    static std::string run(const std::string& command, int* rc = nullptr);\n    static std::vector<std::string> ttyDevices();\n    static std::vector<DetectedReader> detectedReaders();\n    static std::string json(const Json::Value& value);\n''',
    "header discovery declarations",
)

old_tty = '''std::vector<std::string> OscamMiniManager::ttyDevices() {\n    std::vector<std::string> devices;\n    try {\n        if (fs::exists("/dev/serial/by-id")) {\n            for (const auto& entry : fs::directory_iterator("/dev/serial/by-id")) {\n                devices.push_back(entry.path().string());\n            }\n        }\n        for (const auto& entry : fs::directory_iterator("/dev")) {\n            const std::string name = entry.path().filename().string();\n            if (name.rfind("ttyUSB", 0) == 0 || name.rfind("ttyACM", 0) == 0) {\n                devices.push_back(entry.path().string());\n            }\n        }\n    } catch (...) {\n    }\n    std::sort(devices.begin(), devices.end());\n    devices.erase(std::unique(devices.begin(), devices.end()), devices.end());\n    return devices;\n}\n'''
new_tty = r'''std::vector<std::string> OscamMiniManager::ttyDevices() {
    std::vector<std::string> stableDevices;
    std::vector<std::string> fallbackDevices;
    std::set<std::string> representedTargets;

    const auto resolvedKey = [](const fs::path& path) {
        std::error_code ec;
        const fs::path resolved = fs::canonical(path, ec);
        return ec ? path.string() : resolved.string();
    };

    try {
        if (fs::exists("/dev/serial/by-id")) {
            for (const auto& entry : fs::directory_iterator("/dev/serial/by-id")) {
                const std::string path = entry.path().string();
                const std::string target = resolvedKey(entry.path());
                if (representedTargets.insert(target).second) stableDevices.push_back(path);
            }
        }
        if (fs::exists("/dev")) {
            for (const auto& entry : fs::directory_iterator("/dev")) {
                const std::string name = entry.path().filename().string();
                if (name.rfind("ttyUSB", 0) != 0 && name.rfind("ttyACM", 0) != 0) continue;
                const std::string target = resolvedKey(entry.path());
                if (representedTargets.insert(target).second)
                    fallbackDevices.push_back(entry.path().string());
            }
        }
    } catch (...) {
    }

    std::sort(stableDevices.begin(), stableDevices.end());
    std::sort(fallbackDevices.begin(), fallbackDevices.end());
    stableDevices.insert(stableDevices.end(), fallbackDevices.begin(), fallbackDevices.end());
    return stableDevices;
}

std::vector<OscamMiniManager::DetectedReader> OscamMiniManager::detectedReaders() {
    std::vector<DetectedReader> result;

    for (const auto& device : ttyDevices()) {
        DetectedReader reader;
        reader.kind = "serial";
        reader.protocol = "mouse";
        reader.device = device;
        reader.stable = device.rfind("/dev/serial/by-id/", 0) == 0;
        reader.name = reader.stable ? fs::path(device).filename().string() : device;
        std::error_code ec;
        const auto resolved = fs::canonical(device, ec);
        if (!ec && resolved.string() != device) reader.detail = resolved.string();
        if (reader.detail.empty()) reader.detail = reader.stable ? "stable serial path" : "serial device";
        result.push_back(std::move(reader));
    }

    // pcsc_scan is installed by the OSCam-mini package dependencies. It is a
    // discovery helper only: OSCam still opens the selected reader itself by
    // zero-based PC/SC index. Keep this out of statusLocked(), because status
    // refreshes every three seconds and pcsc_scan intentionally stays alive.
    int pcscRc = 0;
    const std::string pcscOutput = run(
        "LC_ALL=C timeout 2s pcsc_scan -n </dev/null", &pcscRc);
    (void)pcscRc; // timeout(1) normally exits 124 after it printed the readers.

    std::set<std::string> pcscSeen;
    std::istringstream lines(pcscOutput);
    std::string line;
    while (std::getline(lines, line)) {
        line = trimLocal(line);
        std::size_t digits = 0;
        while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits])))
            ++digits;
        if (digits == 0 || digits >= line.size() || line[digits] != ':') continue;
        const std::string index = line.substr(0, digits);
        const std::string name = trimLocal(line.substr(digits + 1));
        if (name.empty()) continue;
        const std::string key = index + "\n" + name;
        if (!pcscSeen.insert(key).second) continue;

        DetectedReader reader;
        reader.kind = "pcsc";
        reader.name = name;
        reader.protocol = "pcsc";
        reader.device = index;
        reader.detail = "PC/SC index " + index;
        reader.stable = false;
        result.push_back(std::move(reader));
    }

    return result;
}
'''
replace_once("src/OscamMiniManager.cpp", old_tty, new_tty, "reader discovery implementation")

replace_once(
    "src/OscamMiniManager.cpp",
    '''    result["readers"] = readersJson;\n    return json(result);\n}\n''',
    '''    result["readers"] = readersJson;\n\n    Json::Value detected(Json::arrayValue);\n    for (const auto& reader : detectedReaders()) {\n        Json::Value value;\n        value["kind"] = reader.kind;\n        value["name"] = reader.name;\n        value["protocol"] = reader.protocol;\n        value["device"] = reader.device;\n        value["detail"] = reader.detail;\n        value["stable"] = reader.stable;\n        detected.append(value);\n    }\n    result["detected_readers"] = detected;\n    return json(result);\n}\n''',
    "settings detected readers",
)

replace_once(
    "src/OscamMiniManager.cpp",
    '''                || log.find(reader.label + " [mouse] found card system") != std::string::npos\n                || log.find(reader.label + " [irdeto] THIS WAS A SUCCESSFUL START ATTEMPT") != std::string::npos;\n            const bool error =\n                log.find(reader.label + " [mouse] Error activating card") != std::string::npos\n                || log.find(reader.label + " [mouse] ERROR") != std::string::npos\n                || log.find(reader.label + " [irdeto] ERROR") != std::string::npos\n                || log.find(reader.label + " [mouse] card initializing error") != std::string::npos;\n''',
    '''                || log.find(reader.label + " [mouse] found card system") != std::string::npos\n                || log.find(reader.label + " [pcsc] card detected") != std::string::npos\n                || log.find(reader.label + " [pcsc] found card system") != std::string::npos\n                || log.find(reader.label + " [irdeto] THIS WAS A SUCCESSFUL START ATTEMPT") != std::string::npos;\n            const bool error =\n                log.find(reader.label + " [mouse] Error activating card") != std::string::npos\n                || log.find(reader.label + " [mouse] ERROR") != std::string::npos\n                || log.find(reader.label + " [pcsc] ERROR") != std::string::npos\n                || log.find(reader.label + " [irdeto] ERROR") != std::string::npos\n                || log.find(reader.label + " [mouse] card initializing error") != std::string::npos;\n''',
    "pcsc activity status",
)

replace_once(
    "src/OscamMiniManager.cpp",
    '''<div class="c"><div class="row"><h3 style="margin:0">Phoenix / Smartmouse</h3><button onclick="addReader()">+ Ридер</button></div><div class="hint">Зелёный — карта готова, жёлтый — инициализация/ожидание, красный — неактивен или ошибка.</div><div id="readers"></div></div>\n''',
    '''<div class="c"><div class="row"><h3 style="margin:0">Ридеры OSCam-mini</h3><button onclick="addReader()">+ Ридер</button><button class="alt" onclick="rescanReaders()">Найти ридеры</button><span id="readerScan" class="hint" style="margin:0"></span></div><div class="hint">Автопоиск: стабильные /dev/serial/by-id, ttyUSB/ttyACM и PC/SC (OMNIKEY и совместимые). Выберите найденный ридер или оставьте ручной ввод. Зелёный — карта готова, жёлтый — инициализация/ожидание, красный — неактивен или ошибка.</div><div id="readers"></div></div>\n''',
    "reader card header",
)

replace_once(
    "src/OscamMiniManager.cpp",
    '''let devices=[];\nlet lastStatus={};\n''',
    '''let devices=[];\nlet detectedReaders=[];\nlet lastStatus={};\n''',
    "reader js globals",
)

replace_once(
    "src/OscamMiniManager.cpp",
    '''function addUser(u={}){const box=document.createElement('div');box.innerHTML=userHtml(u,document.querySelectorAll('.user').length);users.append(...box.childNodes);applyActivity(lastStatus)}\n\nfunction readerHtml(r={},i=0){const label=r.label||('Reader'+(i+1));return `<div class="compact-item reader" data-reader="${esc(label)}">\n''',
    '''function addUser(u={}){const box=document.createElement('div');box.innerHTML=userHtml(u,document.querySelectorAll('.user').length);users.append(...box.childNodes);applyActivity(lastStatus)}\n\nfunction detectedReaderOptions(r={}){let match=-1;const out=['<option value="">— ручной ввод —</option>'];detectedReaders.forEach((d,i)=>{const selected=(d.protocol===r.protocol&&String(d.device)===String(r.device));if(selected)match=i;const stable=d.stable?' · stable':'';out.push(`<option value="${i}" ${selected?'selected':''}>${esc(d.name||d.device)} · ${esc((d.protocol||'').toUpperCase())} · ${esc(d.device)}${stable}</option>`)});if(r.device&&match<0)out.push(`<option value="_current" selected>⚠ текущий: ${esc(r.device)} (${esc(r.protocol||'mouse')}) — не найден</option>`);return out.join('')}\nfunction chooseDetectedReader(sel){if(sel.value===''||sel.value==='_current')return;const d=detectedReaders[Number(sel.value)];if(!d)return;const row=sel.closest('.reader');row.querySelector('.reader-device').value=d.device;row.querySelector('.reader-protocol').value=d.protocol;row.querySelector('.reader-summary-device').textContent=(d.name||d.device)+' · '+d.device}\nfunction updateReaderSelectors(){document.querySelectorAll('.reader').forEach(row=>{const r={protocol:row.querySelector('.reader-protocol').value,device:row.querySelector('.reader-device').value};row.querySelector('.reader-detected').innerHTML=detectedReaderOptions(r)});readerScan.textContent='Найдено ридеров: '+detectedReaders.length}\nasync function rescanReaders(){readerScan.textContent='Поиск…';try{const s=await api('/api/oscam-mini/settings');detectedReaders=s.detected_readers||[];updateReaderSelectors()}catch(e){readerScan.textContent='Ошибка поиска: '+e.message}}\n\nfunction readerHtml(r={},i=0){const label=r.label||('Reader'+(i+1));return `<div class="compact-item reader" data-reader="${esc(label)}">\n''',
    "reader discovery js helpers",
)

replace_once(
    "src/OscamMiniManager.cpp",
    '''<div class="compact-body"><div class="g">\n<label>Имя<input class="reader-label" value="${esc(label)}"></label>\n<label>Устройство / индекс PC/SC<input class="reader-device" value="${esc(r.device||'')}" placeholder="/dev/ttyUSB0 или 0"></label>\n<label>CAID<input class="reader-caid" maxlength="4" value="${esc(r.caid||'0652')}"></label>\n''',
    '''<div class="compact-body"><div class="g">\n<label>Имя<input class="reader-label" value="${esc(label)}"></label>\n<label>Найденный ридер<select class="reader-detected" onchange="chooseDetectedReader(this)">${detectedReaderOptions(r)}</select></label>\n<label>Устройство / индекс PC/SC<input class="reader-device" value="${esc(r.device||'')}" placeholder="/dev/serial/by-id/... или 0"></label>\n<label>CAID<input class="reader-caid" maxlength="4" value="${esc(r.caid||'0652')}"></label>\n''',
    "reader discovery select",
)

replace_once(
    "src/OscamMiniManager.cpp",
    '''async function loadAll(){try{const st=await api('/api/oscam-mini/status');lastStatus=st;devices=st.devices||[];state.textContent=st.service_active?'● работает':'● '+(st.service_state||'остановлен');state.className=st.service_active?'status-ok':'status-bad';proc.textContent=st.process||'процесс не запущен';log.textContent=st.log||'';const s=await api('/api/oscam-mini/settings');bind_ip.value=s.bind_ip||'127.0.0.1';key.value=s.key||'';users.innerHTML='';(s.users||[]).forEach(addUser);readers.innerHTML='';(s.readers||[]).forEach(addReader);applyActivity(st)}catch(e){msg.textContent=e.message;msg.className='msg error'}}\n''',
    '''async function loadAll(){try{const st=await api('/api/oscam-mini/status');lastStatus=st;devices=st.devices||[];state.textContent=st.service_active?'● работает':'● '+(st.service_state||'остановлен');state.className=st.service_active?'status-ok':'status-bad';proc.textContent=st.process||'процесс не запущен';log.textContent=st.log||'';const s=await api('/api/oscam-mini/settings');detectedReaders=s.detected_readers||[];bind_ip.value=s.bind_ip||'127.0.0.1';key.value=s.key||'';users.innerHTML='';(s.users||[]).forEach(addUser);readers.innerHTML='';(s.readers||[]).forEach(addReader);readerScan.textContent='Найдено ридеров: '+detectedReaders.length;applyActivity(st)}catch(e){msg.textContent=e.message;msg.className='msg error'}}\n''',
    "load detected readers",
)

replace_once(
    "OSCAM_MINI.md",
    '''The web interface's `oscam.server` editor supports `mouse`, `phoenix`, and\n`pcsc` physical readers and the listed CAID/card families. It continues to\nmanage **only the Newcamd server protocol**, not CCcam/Camd35/Radegast server\nlisteners. Their configuration is not generated by this UI.\n''',
    '''The web interface's `oscam.server` editor supports `mouse`, `phoenix`, and\n`pcsc` physical readers and the listed CAID/card families. It continues to\nmanage **only the Newcamd server protocol**, not CCcam/Camd35/Radegast server\nlisteners. Their configuration is not generated by this UI.\n\n### Automatic reader discovery\n\nThe OSCam-mini page automatically discovers local serial readers from\n`/dev/serial/by-id` (preferred stable names), with `ttyUSB*`/`ttyACM*` as a\nfallback. It also runs a short `pcsc_scan` probe on page load or when **Find\nreaders** is pressed and exposes PC/SC readers by OSCam's zero-based index.\nSelecting a discovered entry fills `protocol` and `device`; manual values remain\navailable. A configured reader that is temporarily unplugged remains visible as\nthe current setting instead of being silently removed.\n''',
    "oscam reader discovery docs",
)
