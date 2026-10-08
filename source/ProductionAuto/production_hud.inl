// ---- production_hud.inl -- Minimal HUD for chain automation status ----


// A tiny semi-transparent, click-through overlay that shows:


//   Line 1: 链式自动化  工作中:N  空闲:M


//   Line 2: 正在生产: 物品名 xN, ...


// Toggled by F5.  No panel, no per-frame rendering -- only refreshed


// after each scan cycle (~20 s) or on F5 toggle.


//


// Data source: directly reads g_productionBindings[] (shared lock)


// and g_productionScanObjects[] (thread_local scan snapshot) to count


// working/idle machines and collect output item names.


// --------------------------------------------------------------------





// ---- HUD state ----


static constexpr wchar_t PRODUCTION_HUD_CLASS[] = L"VillageQoLProductionHUD18";


static constexpr size_t PRODUCTION_HUD_MAX_ROWS = 7;  // item rows (3 items each, max 20 items)


static HWND g_productionHudWindow = nullptr;


static HFONT g_productionHudFont = nullptr;


static bool g_productionHudVisible = false;


// 统一热键（从 qol_hotkeys.txt 读取 productionauto 行）
static QolHotKeys g_productionHudHotkeys = {};


static bool g_productionHudToggleArmed = true;  // edge-detect for HUD toggle key


static wchar_t g_productionHudText[2048] = {};


static HBRUSH g_productionHudBg = nullptr;


static HBRUSH g_productionHudAccent = nullptr;





// ---- DPI scale (reuse the panel's ProductionScale, which is defined


// in production_automation_panel.inl; but that file is no longer included,


// so we define a local equivalent here). ----


static int ProductionHudScale(int value) {


    UINT dpi = 96;


    using GetDpiForWindowFunction = UINT (WINAPI *)(HWND);


    static GetDpiForWindowFunction getDpiForWindow =


        reinterpret_cast<GetDpiForWindowFunction>(


            GetProcAddress(GetModuleHandleW(L"user32.dll"),


                           "GetDpiForWindow"));


    if (getDpiForWindow) {


        HWND owner = FindSettingsOwner();


        if (owner) dpi = getDpiForWindow(owner);


    }


    return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);


}





// ---- UTF-8 to wide helper (independent of panel's version) ----


static void ProductionHudUtf8ToWide(const char* input, wchar_t* output,


                                    size_t outputCount) {


    if (!output || outputCount == 0) return;


    output[0] = L'\0';


    if (!input || !input[0]) return;


    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input, -1,


                        output, static_cast<int>(outputCount));


}





// ---- Convert item ID to display name ----


// Lookup table: item ID -> Chinese name (Unicode escapes, encoding-safe)


static const wchar_t* ProductionItemToName(u64 itemId) {


    if (itemId == 0) return nullptr;


    switch (itemId) {


    // ---- crops (100xxx) ----


    case 100000ULL: return L"\x82A6\x7B0B";


    case 100010ULL: return L"\x6D0B\x8471";


    case 100020ULL: return L"\x8C4C\x8C46";


    case 100030ULL: return L"\x5927\x5934\x83DC";


    case 100040ULL: return L"\x9A6C\x94C3\x85AF";


    case 100050ULL: return L"\x6A31\x6843\x841D\x535C";


    case 100060ULL: return L"\x9AD8\x4E3D\x83DC";


    case 100070ULL: return L"\x83B4\x82E3";


    case 100080ULL: return L"\x7EA2\x841D\x535C";


    case 100090ULL: return L"\x8349\x8393";


    case 100100ULL: return L"\x8471";


    case 100110ULL: return L"\x756A\x8304";


    case 100120ULL: return L"\x8304\x5B50";


    case 100130ULL: return L"\x9752\x6912";


    case 100140ULL: return L"\x5357\x74DC";


    case 100150ULL: return L"\x5C0F\x9EC4\x74DC";


    case 100160ULL: return L"\x7389\x7C73";


    case 100170ULL: return L"\x59DC";


    case 100180ULL: return L"\x5927\x849C";


    case 100190ULL: return L"\x6BDB\x8C46";


    case 100200ULL: return L"\x897F\x74DC";


    case 100210ULL: return L"\x751C\x74DC";


    case 100220ULL: return L"\x51E4\x68A8";


    case 100230ULL: return L"\x84DD\x8393";


    case 100240ULL: return L"\x82F9\x9762";


    case 100250ULL: return L"\x7389\x7C73\x7C73";


    case 100260ULL: return L"\x9ED1\x829D";


    case 100270ULL: return L"\x7EA2\x8C46";


    case 100280ULL: return L"\x828B\x5934";


    case 100290ULL: return L"\x5730\x74DC";


    case 100300ULL: return L"\x9999\x83C7";


    case 100310ULL: return L"\x9E3F\x559C\x83C7";


    case 100320ULL: return L"\x6728\x8033";


    case 100330ULL: return L"\x82B9\x83DC";


    case 100340ULL: return L"\x6CB9\x6A44\x6984";


    case 100350ULL: return L"\x574F\x7C73\x7C73";


    case 100360ULL: return L"\x5927\x767D\x83DC";


    case 100370ULL: return L"\x767D\x841D\x535C";


    case 100380ULL: return L"\x83E0\x83DC";


    case 100390ULL: return L"\x82F9\x9762\x7C73";


    case 100400ULL: return L"\x829D\x9EBB";


    case 100410ULL: return L"\x5C71\x8475";


    case 100420ULL: return L"\x83B2\x85D5";


    case 100430ULL: return L"\x6851\x53F6";


    case 100440ULL: return L"\x6A44\x6984";


    case 100450ULL: return L"\x8C46\x82BD\x83DC";


    case 100460ULL: return L"\x8377\x5170\x8C46";


    // ---- flowers (111xxx) ----


    case 111000ULL: return L"\x82B1\x871C";


    case 111010ULL: return L"\x73AB\x7470";


    case 111020ULL: return L"\x90C1\x91D1\x9999";


    case 111030ULL: return L"\x767D\x73AB\x7470";


    case 111040ULL: return L"\x9EC4\x73AB\x7470";


    case 111050ULL: return L"\x7EA2\x73AB\x7470";


    case 111060ULL: return L"\x8589\x73AB\x7470";


    case 111070ULL: return L"\x7D2B\x73AB\x7470";


    case 111080ULL: return L"\x829D\x83DC\x82B1";


    case 111090ULL: return L"\x73AB\x7470\x82B1";


    case 111100ULL: return L"\x9ED8\x529F\x82B1";


    case 111110ULL: return L"\x767E\x5408\x82B1";


    case 111120ULL: return L"\x9EC4\x5408\x82B1";


    case 111130ULL: return L"\x7EA2\x5408\x82B1";


    case 111140ULL: return L"\x73AB\x7470";


    case 111150ULL: return L"\x8589\x5408\x82B1";


    case 111160ULL: return L"\x6C34\x4ED9\x82B1";


    case 111170ULL: return L"\x90C1\x91D1\x9999";


    case 111180ULL: return L"\x8389\x679D\x8D1E";


    case 111190ULL: return L"\x543C\x8349";


    case 111200ULL: return L"\x83B2\x82B1";


    case 111210ULL: return L"\x83CA\x82B1";


    case 111220ULL: return L"\x7275\x725B\x82B1";


    // ---- fish (122xxx) ----


    case 122000ULL: return L"\x67DA\x5B50";


    case 122010ULL: return L"\x6A31\x6843";


    case 122020ULL: return L"\x6885\x5B50";


    case 122030ULL: return L"\x6843\x5B50";


    case 122040ULL: return L"\x8292\x679C";


    case 122050ULL: return L"\x9999\x8549";


    case 122060ULL: return L"\x53EF\x53EF";


    case 122070ULL: return L"\x8461\x8404";


    case 122080ULL: return L"\x67FF\x5B50";


    case 122090ULL: return L"\x68A8\x5B50";


    case 122100ULL: return L"\x82F9\x679C";


    case 122110ULL: return L"\x6A58\x5B50";


    case 122120ULL: return L"\x67E0\x6AAC";


    // ---- ores (130xxx, 131xxx) ----


    case 130000ULL: return L"\x94C5\x77FF\x539F\x77F3";


    case 130020ULL: return L"\x94DC\x77FF\x77F3";


    case 130040ULL: return L"\x94C1\x77FF\x77F3";


    case 130060ULL: return L"\x91D1\x77FF\x77F3";


    case 130080ULL: return L"\x94F6\x77FF\x539F\x77F3";


    case 130100ULL: return L"\x8C89\x77FF\x539F\x77F3";


    case 130120ULL: return L"\x6C34\x6676\x539F\x77F3";


    case 130140ULL: return L"\x7C9F\x6676\x539F\x77F3";


    case 131000ULL: return L"\x725B\x8089";


    case 131010ULL: return L"\x94DC\x5757";


    case 131020ULL: return L"\x94F6\x5757";


    case 131030ULL: return L"\x94C1\x5757";


    case 131040ULL: return L"\x732A\x8089";


    case 131050ULL: return L"\x6C34\x6676";


    case 131060ULL: return L"\x7C9B\x6676";


    // ---- meat (140xxx) ----


    case 140000ULL: return L"\x732A\x8089";


    case 140010ULL: return L"\x725B\x8089";


    case 140020ULL: return L"\x7F8A\x8089";


    case 140030ULL: return L"\x9E21\x8089";


    case 140040ULL: return L"\x9E2D\x8089";


    case 140050ULL: return L"\x755C\x8089";


    case 140060ULL: return L"\x7565\x8089";


    case 140070ULL: return L"\x853D\x8089";


    case 140100ULL: return L"\x732A\x6392";


    case 140110ULL: return L"\x725B\x6392";


    case 140120ULL: return L"\x7F8A\x6392";


    case 140130ULL: return L"\x9E21\x86CB";


    case 140140ULL: return L"\x9E2D\x86CB";


    case 140150ULL: return L"\x9E7E\x86CB";


    case 140160ULL: return L"\x9E7E\x8089";


    case 140180ULL: return L"\x9C7F\x8089";


    case 140190ULL: return L"\x9C7F\x6392";


    case 140200ULL: return L"\x86C7\x8089";


    case 140220ULL: return L"\x9F99\x867E\x8089";


    case 140230ULL: return L"\x8702\x8089";


    case 140240ULL: return L"\x767D\x8702\x8089";


    case 140250ULL: return L"\x9EC4\x8702\x8089";


    case 140260ULL: return L"\x7EA2\x8702\x8089";


    case 140300ULL: return L"\x9C7F\x8089";


    case 140310ULL: return L"\x9C7F\x6392";


    case 140320ULL: return L"\x9E21\x8089\x4E1D";


    case 140330ULL: return L"\x9E21\x86CB\x9EC4";


    case 140340ULL: return L"\x9E21\x86CB\x767D";


    case 140350ULL: return L"\x9C7C\x8089";


    case 140370ULL: return L"\x6C34\x4EA7\x54C1";


    // ---- animal products (145xxx) ----


    case 145000ULL: return L"\x7F8A\x6BDB";


    case 145020ULL: return L"\x725B\x5976";


    // ---- insects (150xxx) ----


    case 150000ULL: return L"\x871C\x8702";


    case 150010ULL: return L"\x8702\x738B";


    case 150020ULL: return L"\x9E1B\x9E21";


    case 150030ULL: return L"\x5C0F\x9E1B";


    case 150100ULL: return L"\x8702\x738B\x4E73";


    case 150200ULL: return L"\x8702\x80A1";


    case 150300ULL: return L"\x8702\x5DE2";


    case 150310ULL: return L"\x8702\x871C";


    // ---- wood (160xxx) ----


    case 160000ULL: return L"\x677E\x6728";


    case 160010ULL: return L"\x6746\x6728";


    case 160020ULL: return L"\x6A44\x6984\x6728";


    case 160030ULL: return L"\x6851\x6728";


    case 160040ULL: return L"\x7AF9\x5B50";


    case 160050ULL: return L"\x6A2A\x6728";


    case 160060ULL: return L"\x6843\x6728";


    case 160070ULL: return L"\x67DA\x6728";


    case 160080ULL: return L"\x67E0\x6728";


    case 160090ULL: return L"\x67F3\x6728";


    case 160100ULL: return L"\x6A44\x6984\x679A";


    case 160110ULL: return L"\x82B9\x53F6";


    case 160120ULL: return L"\x8336\x53F6";


    case 160130ULL: return L"\x6851\x53F6";


    case 160140ULL: return L"\x8461\x8404\x85E4";


    case 160150ULL: return L"\x7AF9\x53F6";


    case 160160ULL: return L"\x6A2A\x53F6";


    case 160170ULL: return L"\x767D\x677E";


    case 160180ULL: return L"\x7EA2\x677E";


    case 160190ULL: return L"\x9ED1\x677E";


    case 160200ULL: return L"\x677E\x679C";


    case 160210ULL: return L"\x6746\x679C";


    case 160220ULL: return L"\x8589\x679C";


    case 160230ULL: return L"\x67DA\x53F6";


    case 160240ULL: return L"\x8336\x679C";


    case 160250ULL: return L"\x7AF9\x7B0B";


    case 160260ULL: return L"\x82B9\x7C73";


    case 160270ULL: return L"\x68D5\x6988";


    case 160280ULL: return L"\x68D5\x6988\x53F6";


    // ---- special wood (161xxx) ----


    case 161000ULL: return L"\x7279\x6B8A\x677E\x6728";


    case 161010ULL: return L"\x7279\x6B8A\x6746\x6728";


    case 161020ULL: return L"\x7279\x6B8A\x6851\x6728";


    case 161030ULL: return L"\x7279\x6B8A\x6843\x6728";


    case 161040ULL: return L"\x7279\x6B8A\x6A2A\x6728";


    case 161050ULL: return L"\x7279\x6B8A\x67F3\x6728";


    // ---- processed (200xxx) ----


    case 201000ULL: return L"\x4F18\x683C";


    case 201010ULL: return L"\x917F\x917F";


    case 201020ULL: return L"\x5976\x916A";


    case 201030ULL: return L"\x917F\x6CB9";


    case 201500ULL: return L"\x7F8E\x4E43\x6ECB";


    case 207030ULL: return L"\x76D0";


    // ---- fuel/material (211xxx, 221xxx, 230xxx) ----


    case 211000ULL: return L"\x6728\x70AD";


    case 221500ULL: return L"\x751F\x4E1D";


    case 230000ULL: return L"\x6728\x5934";


    case 230001ULL: return L"\x6728\x5934\x4E8C";


    case 230002ULL: return L"\x6728\x5934\x4E09";


    case 230050ULL: return L"\x6728\x6750";


    case 230100ULL: return L"\x6728\x677F";


    // ---- seeds (240xxx) ----


    case 240000ULL: return L"\x6CB9\x6A44\x6984\x79CD\x5B50";


    case 240010ULL: return L"\x67DA\x5B50\x79CD\x5B50";


    case 240020ULL: return L"\x8461\x8404\x79CD\x5B50";


    case 240030ULL: return L"\x6843\x5B50\x79CD\x5B50";


    case 240040ULL: return L"\x67F3\x6811\x79CD\x5B50";


    case 240050ULL: return L"\x7AF9\x5B50\x79CD\x5B50";


    // ---- fertilizer (20010) ----


    case 20010ULL:  return L"\x5806\x80A5";


    case 20020ULL:  return L"\x4F18\x8D28\x5806\x80A5";


    case 20030ULL:  return L"\x7279\x7EA7\x5806\x80A5";


    case 200000ULL: return L"\x814C\x6E0D\x82A6\x7B0B";  // 腌渍土豆 (土豆→腌渍罐)
    case 200010ULL: return L"\x814C\x6E0D\x6D0B\x8471";


    case 200020ULL: return L"\x814C\x6E0D\x8C4C\x8C46";


    case 200030ULL: return L"\x814C\x6E0D\x5927\x5934\x83DC";


    case 200040ULL: return L"\x814C\x6E0D\x9A6C\x94C3\x85AF";


    case 200050ULL: return L"\x814C\x6E0D\x6A31\x6843\x841D\x535C";


    case 200060ULL: return L"\x814C\x6E0D\x9AD8\x4E3D\x83DC";


    case 200070ULL: return L"\x814C\x6E0D\x83B4\x82E3";


    case 200080ULL: return L"\x814C\x6E0D\x7EA2\x841D\x535C";


    case 200090ULL: return L"\x814C\x6E0D\x8471";


    case 200100ULL: return L"\x814C\x6E0D\x756A\x8304";


    case 200110ULL: return L"\x814C\x6E0D\x8304\x5B50";


    case 200120ULL: return L"\x814C\x6E0D\x9752\x6912";


    case 200130ULL: return L"\x814C\x6E0D\x5357\x74DC";


    case 200140ULL: return L"\x814C\x6E0D\x5C0F\x9EC4\x74DC";


    case 200150ULL: return L"\x814C\x6E0D\x7389\x7C73";


    case 200160ULL: return L"\x814C\x6E0D\x59DC";


    case 200170ULL: return L"\x814C\x6E0D\x5927\x849C";


    case 200180ULL: return L"\x814C\x6E0D\x6BDB\x8C46";


    case 200190ULL: return L"\x814C\x6E0D\x828B\x5934";


    case 200200ULL: return L"\x814C\x6E0D\x5730\x74DC";


    case 200210ULL: return L"\x814C\x6E0D\x9999\x83C7";


    case 200220ULL: return L"\x814C\x6E0D\x9E3F\x559C\x83C7";


    case 200230ULL: return L"\x814C\x6E0D\x6728\x8033";


    case 200240ULL: return L"\x814C\x6E0D\x6CB9\x6A44\x6984";


    case 200250ULL: return L"\x814C\x6E0D\x5927\x767D\x83DC";


    case 200260ULL: return L"\x814C\x6E0D\x767D\x841D\x535C";


    case 200270ULL: return L"\x814C\x6E0D\x83E0\x83DC";


    case 200280ULL: return L"\x814C\x6E0D\x5C71\x8475";


    case 200290ULL: return L"\x814C\x6E0D\x83B2\x85D5";


    case 200300ULL: return L"\x814C\x6E0D\x8C46\x82BD\x83DC";


    case 200310ULL: return L"\x814C\x6E0D\x8377\x5170\x8C46";


    case 200400ULL: return L"\x8349\x8393\x679C\x9171";


    case 200410ULL: return L"\x897F\x74DC\x679C\x9171";


    case 200420ULL: return L"\x751C\x74DC\x679C\x9171";


    case 200430ULL: return L"\x51E4\x68A8\x679C\x9171";


    case 200440ULL: return L"\x84DD\x8393\x679C\x9171";


    case 200450ULL: return L"\x67DA\x5B50\x679C\x9171";


    case 200460ULL: return L"\x6A31\x6843\x679C\x9171";


    case 200470ULL: return L"\x6843\x5B50\x679C\x9171";


    case 200480ULL: return L"\x8292\x679C\x679C\x9171";


    case 200490ULL: return L"\x9999\x8549\x679C\x9171";


    case 200500ULL: return L"\x53EF\x53EF\x679C\x9171";


    case 200510ULL: return L"\x8461\x8404\x679C\x9171";


    case 200520ULL: return L"\x67FF\x5B50\x679C\x9171";


    case 200530ULL: return L"\x68A8\x5B50\x679C\x9171";


    case 200540ULL: return L"\x82F9\x679C\x679C\x9171";


    case 200550ULL: return L"\x6A58\x5B50\x679C\x9171";


    case 200560ULL: return L"\x67E0\x6AAC\x679C\x9171";


    case 200580ULL: return L"\x73AB\x7470\x679C\x9171";


    case 200600ULL: return L"\x9178\x6885\x5E72";


    case 200610ULL: return L"\x918B";


    case 200620ULL: return L"\x7EB3\x8C46";


    case 200630ULL: return L"\x4F18\x683C";


    case 200640ULL: return L"\x9C7C\x9732";


    case 202000ULL: return L"\x82A6\x7B0B\x9AD8\x6C64";


    case 202010ULL: return L"\x6D0B\x8471\x9AD8\x6C64";


    case 202020ULL: return L"\x8C4C\x8C46\x9AD8\x6C64";


    case 202030ULL: return L"\x5927\x5934\x83DC\x9AD8\x6C64";


    case 202040ULL: return L"\x9A6C\x94C3\x85AF\x9AD8\x6C64";


    case 202050ULL: return L"\x6A31\x6843\x841D\x535C\x9AD8\x6C64";


    case 202060ULL: return L"\x9AD8\x4E3D\x83DC\x9AD8\x6C64";


    case 202070ULL: return L"\x83B4\x82E3\x9AD8\x6C64";


    case 202080ULL: return L"\x7EA2\x841D\x535C\x9AD8\x6C64";


    case 202090ULL: return L"\x8471\x9AD8\x6C64";


    case 202100ULL: return L"\x756A\x8304\x9AD8\x6C64";


    case 202110ULL: return L"\x8304\x5B50\x9AD8\x6C64";


    case 202120ULL: return L"\x9752\x6912\x9AD8\x6C64";


    case 202130ULL: return L"\x5357\x74DC\x9AD8\x6C64";


    case 202140ULL: return L"\x5C0F\x9EC4\x74DC\x9AD8\x6C64";


    case 202150ULL: return L"\x7389\x7C73\x9AD8\x6C64";


    case 202160ULL: return L"\x59DC\x9AD8\x6C64";


    case 202170ULL: return L"\x5927\x849C\x9AD8\x6C64";


    case 202180ULL: return L"\x6BDB\x8C46\x9AD8\x6C64";


    case 202190ULL: return L"\x828B\x5934\x9AD8\x6C64";


    case 202200ULL: return L"\x9999\x83C7\x9AD8\x6C64";


    case 202210ULL: return L"\x9E3F\x559C\x83C7\x9AD8\x6C64";


    case 202220ULL: return L"\x6728\x8033\x9AD8\x6C64";


    case 202230ULL: return L"\x6CB9\x6A44\x6984\x9AD8\x6C64";


    case 202240ULL: return L"\x5927\x767D\x83DC\x9AD8\x6C64";


    case 202250ULL: return L"\x767D\x841D\x535C\x9AD8\x6C64";


    case 202260ULL: return L"\x83E0\x83DC\x9AD8\x6C64";


    case 202270ULL: return L"\x5C71\x8475\x9AD8\x6C64";


    case 202280ULL: return L"\x83B2\x85D5\x9AD8\x6C64";


    case 203000ULL: return L"\x8349\x8393\x84B8\x998F\x9152";


    case 203010ULL: return L"\x897F\x74DC\x84B8\x998F\x9152";


    case 203020ULL: return L"\x751C\x74DC\x84B8\x998F\x9152";


    case 203030ULL: return L"\x51E4\x68A8\x84B8\x998F\x9152";


    case 203040ULL: return L"\x84DD\x8393\x84B8\x998F\x9152";


    case 203050ULL: return L"\x67DA\x5B50\x84B8\x998F\x9152";


    case 203060ULL: return L"\x6A31\x6843\x84B8\x998F\x9152";


    case 203070ULL: return L"\x6843\x5B50\x84B8\x998F\x9152";


    case 203080ULL: return L"\x8292\x679C\x84B8\x998F\x9152";


    case 203090ULL: return L"\x9999\x8549\x84B8\x998F\x9152";


    case 203100ULL: return L"\x53EF\x53EF\x84B8\x998F\x9152";


    case 203110ULL: return L"\x67FF\x5B50\x84B8\x998F\x9152";


    case 203120ULL: return L"\x68A8\x5B50\x84B8\x998F\x9152";


    case 203130ULL: return L"\x82F9\x679C\x84B8\x998F\x9152";


    case 203140ULL: return L"\x6A58\x5B50\x84B8\x998F\x9152";


    case 203150ULL: return L"\x67E0\x6AAC\x84B8\x998F\x9152";


    case 203200ULL: return L"\x8461\x8404\x9152";


    case 203210ULL: return L"\x6885\x9152";


    case 203220ULL: return L"\x5564\x9152";


    case 203230ULL: return L"\x65E5\x672C\x9152";


    case 203240ULL: return L"\x9EA6\x70E7\x914E";


    case 203250ULL: return L"\x5730\x74DC\x70E7\x914E";


    case 204000ULL: return L"\x719F\x6210\x8089";


    case 204010ULL: return L"\x5473\x564C";


    case 205500ULL: return L"\x8349\x8393\x5E72";


    case 205510ULL: return L"\x897F\x74DC\x5E72";


    case 205520ULL: return L"\x751C\x74DC\x5E72";


    case 205530ULL: return L"\x51E4\x68A8\x5E72";


    case 205540ULL: return L"\x84DD\x8393\x5E72";


    case 205550ULL: return L"\x67DA\x5B50\x5E72";


    case 205560ULL: return L"\x6A31\x6843\x5E72";


    case 205570ULL: return L"\x6843\x5B50\x5E72";


    case 205580ULL: return L"\x8292\x679C\x5E72";


    case 205590ULL: return L"\x9999\x8549\x5E72";


    case 205600ULL: return L"\x8461\x8404\x5E72";


    case 205610ULL: return L"\x67FF\x997C";


    case 205620ULL: return L"\x68A8\x5B50\x5E72";


    case 205630ULL: return L"\x82F9\x679C\x5E72";


    case 205640ULL: return L"\x6A58\x5B50\x5E72";


    case 205650ULL: return L"\x67E0\x6AAC\x5E72";


    case 206000ULL: return L"\x5E72\x8D27";


    case 206010ULL: return L"\x8089\x5E72";


    case 206020ULL: return L"\x725B\x8089\x5E72";


    case 206030ULL: return L"\x732A\x8089\x5E72";


    case 206040ULL: return L"\x5E72\x71E5\x9999\x83C7";


    case 206050ULL: return L"\x5E72\x71E5\x9E3F\x559C\x83C7";


    case 206060ULL: return L"\x5E72\x71E5\x6728\x8033";


    case 206500ULL: return L"\x5E72\x71E5\x82B1";


    case 207000ULL: return L"\x835E\x9EA6\x7C89";


    case 207010ULL: return L"\x9762\x7C89";


    case 207020ULL: return L"\x62B9\x8336";


    case 207040ULL: return L"\x7802\x7CD6";


    case 207050ULL: return L"\x9EC4\x8C46\x7C89";


    case 208000ULL: return L"\x918B";


    case 208010ULL: return L"\x57F9\x6839";


    case 208020ULL: return L"\x70DF\x718F\x9C7C";


    case 208030ULL: return L"\x70DF\x718F\x8D77\x53F8";


    case 208040ULL: return L"\x70DF\x718F\x86CB";


    case 208050ULL: return L"\x70DF\x718F\x9E21\x8089";


    case 208060ULL: return L"\x706B\x9E21\x817F";


    case 209000ULL: return L"\x6CB9";


    case 209010ULL: return L"\x829D\x9EBB\x6CB9";


    case 209020ULL: return L"\x6A44\x6984\x6CB9";


    case 209030ULL: return L"\x677E\x9732\x6CB9";


    case 209040ULL: return L"\x9171\x6CB9";


    case 209100ULL: return L"\x7EA2\x8272\x67D3\x6599";


    case 209110ULL: return L"\x6A58\x8272\x67D3\x6599";


    case 209120ULL: return L"\x9EC4\x8272\x67D3\x6599";


    case 209130ULL: return L"\x7EFF\x8272\x67D3\x6599";


    case 209140ULL: return L"\x84DD\x8272\x67D3\x6599";


    case 209150ULL: return L"\x7D2B\x8272\x67D3\x6599";


    case 209160ULL: return L"\x9ED1\x8272\x67D3\x6599";


    case 209170ULL: return L"\x767D\x8272\x67D3\x6599";


    case 209180ULL: return L"\x7C89\x7EA2\x8272\x67D3\x6599";


    case 209190ULL: return L"\x5496\x5561\x8272\x67D3\x6599";


    case 210000ULL: return L"\x73BB\x7483\x68D2";


    case 210010ULL: return L"\x94DC\x6761";


    case 210020ULL: return L"\x94C1\x6761";


    case 210030ULL: return L"\x91D1\x6761";


    case 210040ULL: return L"\x9668\x94C1\x6761";


    case 210060ULL: return L"\x7816\x5934";


    case 212000ULL: return L"\x4E1D\x7EF8";


    case 212010ULL: return L"\x5E03";


    case 212020ULL: return L"\x5580\x4EC0\x7C73\x5C14\x6BDB\x5E03";


    case 221000ULL: return L"\x8702\x871C";


    case 221010ULL: return L"\x8702\x738B\x4E73";  // 蜂王浆 (蜂蜜加工)



    default: return nullptr;


    }


}





static void ProductionHudItemName(


        const ProductionObjectSnapshot* machine,


        wchar_t* output, size_t outputCount) {


    if (!output || outputCount == 0) return;


    output[0] = L'\0';


    if (!machine) return;


    // Try to find a running slot with outputItemId


    for (size_t slot = 0; slot < PRODUCTION_MACHINE_SLOTS; ++slot) {


        if (machine->outputItemId[slot] && machine->outputCount[slot] > 0) {


            // First try the item name lookup table


            const wchar_t* itemName = ProductionItemToName(


                machine->outputItemId[slot]);


            if (itemName) {


                wcscpy_s(output, outputCount, itemName);


            } else if (machine->named && machine->name[0]) {


                ProductionHudUtf8ToWide(machine->name, output, outputCount);


            } else {


                _snwprintf_s(output, outputCount, _TRUNCATE,


                             L"item#%llu",


                             static_cast<unsigned long long>(


                                 machine->outputItemId[slot]));


            }


            return;


        }


    }


    // No active output slot: use machine name if available


    if (machine->named && machine->name[0]) {


        ProductionHudUtf8ToWide(machine->name, output, outputCount);


    } else {


        wcscpy_s(output, outputCount, L"unknown");


    }


}





// ---- Collect HUD data from bindings + scan objects ----


struct ProductionHudStats {


    size_t workingCount;   // Running, Starting, OutputReady


    size_t idleCount;      // Idle, WaitingInput


    size_t faultedCount;   // Faulted, Unbound


    // Top producing items (by outputItemId, aggregated across machines)


    static constexpr size_t MAX_ITEMS = 20;


    u64 itemIds[MAX_ITEMS];


    int itemCounts[MAX_ITEMS];


    wchar_t itemNames[MAX_ITEMS][48];


    size_t itemCount;


};





static void ProductionHudCollect(ProductionHudStats& stats) {


    stats.workingCount = 0;


    stats.idleCount = 0;


    stats.faultedCount = 0;


    stats.itemCount = 0;


    for (size_t i = 0; i < ProductionHudStats::MAX_ITEMS; ++i) {


        stats.itemIds[i] = 0;


        stats.itemCounts[i] = 0;


        stats.itemNames[i][0] = L'\0';


    }





    // We need the scan objects to resolve machine names for output items.


    // g_productionScanObjects is thread_local on the scan thread.  The HUD


    // refresh is called from the same thread right after ProductionPublish,


    // so the scan objects are still valid.


    const ProductionObjectSnapshot* objects = g_productionScanObjects;


    const size_t objectCount = g_productionScanObjectCount;





    AcquireSRWLockShared(&g_productionBindingLock);


    for (size_t i = 0; i < g_productionBindingCount; ++i) {


        const ProductionBinding& b = g_productionBindings[i];


        switch (b.state) {


        case ProductionState::Running:


        case ProductionState::Starting:


        case ProductionState::OutputReady:


        case ProductionState::WaitingOutputSpace:


            ++stats.workingCount;


            break;


        case ProductionState::Idle:


        case ProductionState::WaitingInput:


            ++stats.idleCount;


            break;


        case ProductionState::Faulted:


        case ProductionState::Unbound:


            ++stats.faultedCount;


            break;


        }





        // Collect output items from running machines


        if ((b.state == ProductionState::Running ||


             b.state == ProductionState::Starting ||


             b.state == ProductionState::OutputReady) &&


            stats.itemCount < ProductionHudStats::MAX_ITEMS) {


            // Find the machine in scan objects


            const ProductionObjectSnapshot* machine = nullptr;


            for (size_t oi = 0; oi < objectCount; ++oi) {


                if (ProductionIdEqual(objects[oi].id, b.machine)) {


                    machine = &objects[oi];


                    break;


                }


            }


            if (machine) {


                // Find the active output slot


                for (size_t slot = 0; slot < PRODUCTION_MACHINE_SLOTS;


                     ++slot) {


                    if (machine->outputItemId[slot] &&


                        machine->outputCount[slot] > 0 &&


                        machine->start[slot]) {


                        u64 itemId = machine->outputItemId[slot];


                        // Check if this item is already in our list


                        bool found = false;


                        for (size_t ki = 0; ki < stats.itemCount; ++ki) {


                            if (stats.itemIds[ki] == itemId) {


                                stats.itemCounts[ki] +=


                                    machine->outputCount[slot];


                                found = true;


                                break;


                            }


                        }


                        if (!found &&


                            stats.itemCount < ProductionHudStats::MAX_ITEMS) {


                            stats.itemIds[stats.itemCount] = itemId;


                            stats.itemCounts[stats.itemCount] =


                                machine->outputCount[slot];


                            ProductionHudItemName(machine,


                                stats.itemNames[stats.itemCount], 48);


                            ++stats.itemCount;


                        }


                        break;  // only first active slot per machine


                    }


                }


            }


        }


    }


    ReleaseSRWLockShared(&g_productionBindingLock);


}





// ---- Format HUD text ----


static void ProductionHudFormat(const ProductionHudStats& stats) {


    if (stats.workingCount == 0 && stats.idleCount == 0 &&


        stats.faultedCount == 0) {


        _snwprintf_s(g_productionHudText,


            sizeof(g_productionHudText) / sizeof(g_productionHudText[0]),


            _TRUNCATE, L"\u94fe\u5f0f\u81ea\u52a8\u5316  \u65e0\u7ed1\u5b9a\u673a\u5668");


        return;


    }





    // Line 1: 链式自动化  工作中:N  空闲:M


    // Line 2: 正在生产: 物品名 xN, ...


    wchar_t line1[128] = {};


    _snwprintf_s(line1, sizeof(line1) / sizeof(line1[0]), _TRUNCATE,


                 L"\u94fe\u5f0f\u81ea\u52a8\u5316  "


                 L"\u5de5\u4f5c\u4e2d:%zu  \u7a7a\u95f2:%zu%s",


                 stats.workingCount, stats.idleCount,


                 stats.faultedCount > 0 ? L"  \u6545\u969c" : L"");





    if (stats.itemCount == 0) {


        _snwprintf_s(g_productionHudText,


            sizeof(g_productionHudText) / sizeof(g_productionHudText[0]),


            _TRUNCATE, L"%ls\n\u6b63\u5728\u751f\u4ea7: --",


            line1);


    } else {


        // Items are laid out two per row, rows separated by '\n'.


        // The window width is derived from the widest row (see


        // ProductionHudMeasureSize), so rows stay roughly equal width.


        wchar_t rows[PRODUCTION_HUD_MAX_ROWS][200] = {};


        size_t rowIndex = 0;


        for (size_t i = 0; i < stats.itemCount; ++i) {


            const size_t slot = i % 3;  // 0 = first item of the row (3 per row)


            if (slot == 0) {


                _snwprintf_s(rows[rowIndex],


                    sizeof(rows[rowIndex]) / sizeof(rows[rowIndex][0]),


                    _TRUNCATE, L"\u6b63\u5728\u751f\u4ea7: ");


            }


            const wchar_t* sep = (slot == 0) ? L"" : L"\u3001";


            _snwprintf_s(rows[rowIndex] + wcslen(rows[rowIndex]),


                sizeof(rows[rowIndex]) / sizeof(rows[rowIndex][0]) -


                    wcslen(rows[rowIndex]),


                _TRUNCATE, L"%ls%ls x%d",


                sep, stats.itemNames[i], stats.itemCounts[i]);


            if (slot == 2 || i + 1 == stats.itemCount) {


                ++rowIndex;


                if (rowIndex >= PRODUCTION_HUD_MAX_ROWS) break;


            }


        }


        // Join rows into g_productionHudText


        const size_t hudCap = sizeof(g_productionHudText) /


                              sizeof(g_productionHudText[0]);


        _snwprintf_s(g_productionHudText, hudCap, _TRUNCATE,


                     L"%ls\n%ls", line1, rows[0]);


        for (size_t r = 1; r < rowIndex; ++r) {


            size_t len = wcslen(g_productionHudText);


            if (len + 1 >= hudCap) break;  // buffer full


            _snwprintf_s(g_productionHudText + len, hudCap - len,


                         _TRUNCATE, L"\n%ls", rows[r]);


        }


    }


}





// ---- Window proc ----


static LRESULT CALLBACK ProductionHudWndProc(HWND window, UINT message,


                                               WPARAM wParam, LPARAM lParam) {


    switch (message) {


    case WM_NCHITTEST:


        return HTTRANSPARENT;


    case WM_ERASEBKGND:


        return 1;


    case WM_PAINT: {


        PAINTSTRUCT paint = {};


        HDC dc = BeginPaint(window, &paint);


        RECT client = {};


        GetClientRect(window, &client);


        if (!g_productionHudBg)


            g_productionHudBg = CreateSolidBrush(RGB(22, 25, 30));


        FillRect(dc, &client, g_productionHudBg);


        RECT accent = client;


        accent.right = accent.left + ProductionHudScale(4);


        if (!g_productionHudAccent)


            g_productionHudAccent = CreateSolidBrush(RGB(80, 205, 118));


        FillRect(dc, &accent, g_productionHudAccent);


        RECT textRect = client;


        textRect.left += ProductionHudScale(14);


        textRect.right -= ProductionHudScale(10);


        textRect.top += ProductionHudScale(4);


        HFONT oldFont = reinterpret_cast<HFONT>(


            SelectObject(dc, g_productionHudFont));


        SetBkMode(dc, TRANSPARENT);


        SetTextColor(dc, RGB(246, 247, 249));


        DrawTextW(dc, g_productionHudText, -1, &textRect,


                  DT_LEFT | DT_TOP | DT_NOPREFIX);


        SelectObject(dc, oldFont);


        EndPaint(window, &paint);


        return 0;


    }


    default:


        return DefWindowProcW(window, message, wParam, lParam);


    }


}





// ---- Measure the size the current text needs (auto width + height) ----


// Uses DT_CALCRECT with no width limit, so the window tightly fits the


// widest row.  Rows are already formatted two items each (see Format),


// so the width is driven by the longest item name pair.


static void ProductionHudMeasureSize(int& outWidth, int& outHeight) {


    int fallbackW = ProductionHudScale(200);


    int fallbackH = ProductionHudScale(52);


    if (!g_productionHudWindow || !g_productionHudFont) {


        outWidth = fallbackW;


        outHeight = fallbackH;


        return;


    }


    HDC dc = GetDC(g_productionHudWindow);


    if (!dc) {


        outWidth = fallbackW;


        outHeight = fallbackH;


        return;


    }


    HFONT oldFont = reinterpret_cast<HFONT>(


        SelectObject(dc, g_productionHudFont));


    RECT textRect = {};


    textRect.left = 0;


    textRect.top = 0;


    textRect.right = 8192;  // large limit: DT_CALCRECT expands to real width


    DrawTextW(dc, g_productionHudText, -1, &textRect,


              DT_LEFT | DT_TOP | DT_NOPREFIX | DT_CALCRECT);


    SelectObject(dc, oldFont);


    ReleaseDC(g_productionHudWindow, dc);


    int textW = textRect.right - textRect.left;


    int textH = textRect.bottom - textRect.top;


    // Window = text + left pad 14 + right pad 10 + top/bottom 4 each


    int w = textW + ProductionHudScale(14) + ProductionHudScale(10);


    int h = textH + ProductionHudScale(8);


    if (w < ProductionHudScale(200)) w = ProductionHudScale(200);


    if (h < ProductionHudScale(32)) h = ProductionHudScale(32);


    outWidth = w;


    outHeight = h;


}





// ---- Initialize HUD window ----


static bool ProductionHudInitWindow() {


    if (g_productionHudWindow) return true;


    WNDCLASSEXW cls = {};


    cls.cbSize = sizeof(cls);


    cls.lpfnWndProc = ProductionHudWndProc;


    cls.hInstance = g_module;


    cls.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));


    cls.lpszClassName = PRODUCTION_HUD_CLASS;


    if (!RegisterClassExW(&cls)) {


        WNDCLASSEXW existing = {};


        existing.cbSize = sizeof(existing);


        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS ||


            !GetClassInfoExW(g_module, PRODUCTION_HUD_CLASS, &existing) ||


            existing.lpfnWndProc != ProductionHudWndProc ||


            existing.hInstance != g_module) {


            Log("[ProductionAuto][HUD] RegisterClassExW failed err=%lu\n",


                GetLastError());


            return false;


        }


    }


    g_productionHudFont = CreateFontW(


        -ProductionHudScale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,


        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,


        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,


        L"Microsoft YaHei UI");


    if (!g_productionHudFont) {


        Log("[ProductionAuto][HUD] CreateFontW failed err=%lu\n",


            GetLastError());


        return false;


    }


    int width = 0;


    int height = 0;


    ProductionHudMeasureSize(width, height);


    g_productionHudWindow = CreateWindowExW(


        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |


            WS_EX_LAYERED | WS_EX_TRANSPARENT,


        PRODUCTION_HUD_CLASS, L"", WS_POPUP, 0, 0,


        width, height,


        nullptr, nullptr, g_module, nullptr);


    if (!g_productionHudWindow) {


        Log("[ProductionAuto][HUD] CreateWindowExW failed err=%lu\n",


            GetLastError());


        DeleteObject(g_productionHudFont);


        g_productionHudFont = nullptr;


        return false;


    }


    SetLayeredWindowAttributes(g_productionHudWindow, 0, 235, LWA_ALPHA);


    Log("[ProductionAuto][HUD] InitWindow OK window=%p font=%p\n",


        g_productionHudWindow, g_productionHudFont);


    return true;


}





// ---- Position HUD at bottom-left corner of the game window ----


static void ProductionHudPosition() {


    if (!g_productionHudWindow) return;


    HWND owner = FindSettingsOwner();


    RECT anchor = {};


    if (!owner || !GetWindowRect(owner, &anchor)) {


        SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0);


    }


    MONITORINFO monitor = {};


    monitor.cbSize = sizeof(monitor);


    HMONITOR handle = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);


    if (!GetMonitorInfoW(handle, &monitor)) monitor.rcWork = anchor;


    int width = 0;


    int height = 0;


    ProductionHudMeasureSize(width, height);


    // Bottom-left corner with 16px margin


    int x = monitor.rcWork.left + ProductionHudScale(16);


    int y = monitor.rcWork.bottom - height - ProductionHudScale(16);


    SetWindowPos(g_productionHudWindow, HWND_TOPMOST, x, y,


                width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);


    Log("[ProductionAuto][HUD] Pos: owner=%p work=(%ld,%ld)-(%ld,%ld) win=%dx%d at (%d,%d) visible=%d\n",


        owner,


        monitor.rcWork.left, monitor.rcWork.top,


        monitor.rcWork.right, monitor.rcWork.bottom,


        width, height, x, y,


        IsWindowVisible(g_productionHudWindow) ? 1 : 0);


}





// ---- Refresh HUD content (called after scan completion) ----


static void ProductionHudRefresh() {


    if (!g_productionHudVisible || !g_productionHudWindow) return;


    ProductionHudStats stats = {};


    ProductionHudCollect(stats);


    ProductionHudFormat(stats);


    // Re-fit the window size to the new text (auto width + height).


    // Bottom-left corner stays anchored.


    ProductionHudPosition();


    InvalidateRect(g_productionHudWindow, nullptr, TRUE);


}





// ---- Show / hide HUD ----


static void ProductionHudShow() {


    if (!ProductionHudInitWindow()) {


        Log("[ProductionAuto][HUD] InitWindow FAILED\n");


        return;


    }


    ProductionHudPosition();


    g_productionHudVisible = true;


    ProductionHudRefresh();


    Log("[ProductionAuto][HUD] Show: window=%p visible=1\n",


        g_productionHudWindow);


}





static void ProductionHudHide() {


    if (g_productionHudWindow && IsWindowVisible(g_productionHudWindow))


        ShowWindow(g_productionHudWindow, SW_HIDE);


    g_productionHudVisible = false;


}





static void ProductionHudToggle() {


    if (g_productionHudVisible)


        ProductionHudHide();


    else


        ProductionHudShow();


}





// ---- Pump window messages (must be called every tick for WM_PAINT) ----


static void ProductionHudPumpMessages() {


    if (!g_productionHudWindow) return;


    MSG msg = {};


    while (PeekMessageW(&msg, g_productionHudWindow, 0, 0, PM_REMOVE)) {


        TranslateMessage(&msg);


        DispatchMessageW(&msg);


    }


}





// ---- Poll F5 key in mod_tick (edge-detected) ----


static void ProductionHudPoll() {


    // Throttle to ~100ms to reduce per-tick overhead.


    static DWORD lastPollTick = 0;


    DWORD now = (DWORD)GetTickCount64();


    if (lastPollTick && (now - lastPollTick) < 100) return;


    lastPollTick = now;





    // Pump window messages (required for WM_PAINT to fire)


    ProductionHudPumpMessages();

    // 热键统一机制：从 qol_hotkeys.txt 读取（支持游戏内改键）
    QolHotkeyCheckReload(&g_productionHudHotkeys);
    const bool f5Down = (GetAsyncKeyState(
        QolHotKeysVk(&g_productionHudHotkeys, 0)) & 0x8000) != 0;


    if (!f5Down) g_productionHudToggleArmed = true;


    if (f5Down && g_productionHudToggleArmed) {


        g_productionHudToggleArmed = false;


        ProductionHudToggle();


        Log("[ProductionAuto][HUD] F5 toggle, visible=%d window=%p\n",


            g_productionHudVisible ? 1 : 0,


            g_productionHudWindow);


    }


}





// ---- Cleanup on unload ----


static void ProductionHudCleanup() {


    if (g_productionHudWindow) {


        ShowWindow(g_productionHudWindow, SW_HIDE);


        DestroyWindow(g_productionHudWindow);


        g_productionHudWindow = nullptr;


    }


    if (g_productionHudFont) {


        DeleteObject(g_productionHudFont);


        g_productionHudFont = nullptr;


    }


    if (g_productionHudBg) {


        DeleteObject(g_productionHudBg);


        g_productionHudBg = nullptr;


    }


    if (g_productionHudAccent) {


        DeleteObject(g_productionHudAccent);


        g_productionHudAccent = nullptr;


    }


    g_productionHudVisible = false;


}


