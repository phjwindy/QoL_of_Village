// production_hud_v2.inl -- ProductionAuto v2 精简 HUD（F5 切换）
//
// 替代旧版 production_hud.inl（1731行）+ panel（2160行）：
// 半透明点击穿透覆盖层，显示：
//   链式自动化  组:N  工作:M  空闲:K
//   正在生产: 物品名 xN, ...
// F5 切换。无面板、无逐帧渲染——仅扫描周期（5s）或切换时刷新。
//
// 作者：PHJ&消失的清风  2026-10-09

#pragma once
#include "game_window.h"  // QolFindGameWindow（HUD 定位）

static constexpr wchar_t PA_HUD_CLASS[] = L"VillageQoLPAHUDv2";
static HWND g_paHudWindow = nullptr;
static HFONT g_paHudFont = nullptr;
static bool g_paHudVisible = false;
static QolHotKeys g_paHudHotkeys = {};
static bool g_paHudToggleArmed = true;
static wchar_t g_paHudText[1024] = {};
static HBRUSH g_paHudBg = nullptr;
static HBRUSH g_paHudAccent = nullptr;

static int PaHudScale(int value) {
    UINT dpi = 96;
    using GetDpiForWindowFunction = UINT (WINAPI *)(HWND);
    static GetDpiForWindowFunction getDpi =
        reinterpret_cast<GetDpiForWindowFunction>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"),
                           "GetDpiForWindow"));
    // v1.6 修正：缓存设置宿主窗口，避免每次调用都 EnumWindows 扫描（原 PaHudScale
    //   被 PaHudPosition/PaHudFormat 等每帧多次调用，每次枚举顶层窗口开销明显，
    //   高 DPI 下更易出现缩放抖动）。宿主失效（IsWindow 为假）时才重新查找。
    static HWND s_owner = nullptr;
    if (!s_owner || !IsWindow(s_owner)) s_owner = FindSettingsOwner();
    HWND owner = s_owner;
    if (getDpi && owner) dpi = getDpi(owner);
    return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}

static void PaHudUtf8ToWide(const char* input, wchar_t* output,
                            size_t outLen) {
    if (!input || !output || outLen == 0) return;
    MultiByteToWideChar(CP_UTF8, 0, input, -1, output,
                        static_cast<int>(outLen));
    output[outLen - 1] = 0;
}

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

    // v1.5.113 纠正：230100 是【煤炭】不是"木板"。
    //   证据三条：① data.dat 里 OBJECT_COAL=石炭(煤炭)、CHARCOAL=木炭；
    //   ② 全配方表无任何配方产出 230100（木板必然由木材加工而来）；
    //   ③ 黑色染料配方"从煤炭、木炭跟小动物的骨头萃取"= in 230100 出 3 个 /
    //      in 211000 出 1 个，且高炉的燃料变体恰为 {211000, 230100}。
    case 230100ULL: return L"\x7164\x70AD";

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

// ---- 数据收集（从 v2 全局状态）----
static void PaHudCollect(size_t* groupsOut, size_t* workingOut,
                         size_t* idleOut, wchar_t* itemsText,
                         size_t itemsLen) {
    size_t groups = 0, working = 0, idle = 0;
    // 合并相同物品：id + 名称 + 累计数量（最多 8 种）
    u64 itemIds[8] = {};
    wchar_t itemNames[8][24] = {};
    int itemCounts[8] = {};
    int itemCount = 0;
    if (itemsText && itemsLen > 0) itemsText[0] = 0;

    // gameSecond 用于机器状态分类
    std::int64_t gameSecond = 0;
    if (g_productionMainWorldGameClockSourceReady.load(
            std::memory_order_acquire)) {
        gameSecond = g_productionMainWorldGameSecond.load(
            std::memory_order_relaxed);
    }

    for (size_t gi = 0; gi < g_groupCount; ++gi) {
        PaGroup* g = &g_groups[gi];
        groups++;
        for (size_t m = 0; m < g->machineCount; ++m) {
            PaGimmickEntry* machine = nullptr;
            for (size_t e = 0; e < PA_MAX_ENTRIES; ++e) {
                if (g_entries[e].id.uniqueId != 0 &&
                    PaIdEqual(g_entries[e].id, g->machines[m])) {
                    machine = &g_entries[e];
                    break;
                }
            }
            if (!machine) continue;
            PaState st = PaState::Idle;
            size_t readySlot = SIZE_MAX;
            bool hasIdle = false;
            if (!PaClassifyMachine(machine->status, gameSecond, &st,
                                   &readySlot, &hasIdle))
                continue;
            if (st == PaState::Running) {
                working++;
                for (size_t slot = 0; slot < PA_MACHINE_SLOTS; ++slot) {
                    char idKey[8] = {};
                    _snprintf_s(idKey, sizeof(idKey), _TRUNCATE,
                                "itemID%zu", slot);
                    char cntKey[9] = {};
                    _snprintf_s(cntKey, sizeof(cntKey), _TRUNCATE,
                                "itemVal%zu", slot);
                    u64 outId = 0;
                    int outCount = 0;
                    if (!PaReadStatusU64(machine->status, idKey, &outId) ||
                        outId == 0) continue;
                    PaReadStatusInt(machine->status, cntKey, &outCount);
                    // 合并相同物品
                    int idx = -1;
                    for (int k = 0; k < itemCount; ++k) {
                        if (itemIds[k] == outId) { idx = k; break; }
                    }
                    if (idx < 0) {
                        if (itemCount >= 8) continue;
                        idx = itemCount++;
                        itemIds[idx] = outId;
                        itemCounts[idx] = 0;
                        const wchar_t* nm = ProductionItemToName(outId);
                        if (nm) wcsncpy_s(itemNames[idx], 24, nm, _TRUNCATE);
                        else swprintf_s(itemNames[idx], 24, L"%ll",
                                       static_cast<unsigned long long>(outId));
                    }
                    itemCounts[idx] += outCount;
                }
            } else if (st == PaState::Ready) {
                working++;
                if (readySlot != SIZE_MAX) {
                    char idKey[8] = {};
                    _snprintf_s(idKey, sizeof(idKey), _TRUNCATE,
                                "itemID%zu", readySlot);
                    char cntKey[9] = {};
                    _snprintf_s(cntKey, sizeof(cntKey), _TRUNCATE,
                                "itemVal%zu", readySlot);
                    u64 outId = 0;
                    int outCount = 0;
                    if (PaReadStatusU64(machine->status, idKey, &outId) &&
                        outId != 0) {
                        PaReadStatusInt(machine->status, cntKey, &outCount);
                        int idx = -1;
                        for (int k = 0; k < itemCount; ++k) {
                            if (itemIds[k] == outId) { idx = k; break; }
                        }
                        if (idx < 0) {
                            if (itemCount >= 8) continue;
                            idx = itemCount++;
                            itemIds[idx] = outId;
                            itemCounts[idx] = 0;
                            const wchar_t* name = ProductionItemToName(outId);
                            if (name) wcsncpy_s(itemNames[idx], 24, name, _TRUNCATE);
                            else swprintf_s(itemNames[idx], 24, L"%ll",
                                           static_cast<unsigned long long>(outId));
                        }
                        itemCounts[idx] += outCount;
                    }
                }
            } else {
                idle++;
            }
        }
    }
    if (groupsOut) *groupsOut = groups;
    if (workingOut) *workingOut = working;
    if (idleOut) *idleOut = idle;
    // 拼接："名字*数量, 名字*数量" 每行 4 个换行
    if (itemsText && itemsLen > 0 && itemCount > 0) {
        wchar_t* p = itemsText;
        size_t remain = itemsLen;
        for (int i = 0; i < itemCount && remain > 1; ++i) {
            wchar_t seg[40];
            _snwprintf_s(seg, 40, L"%ls*%d", itemNames[i], itemCounts[i]);
            const size_t len = wcslen(seg);
            if (i > 0 && remain > 3) {
                if (i % 4 == 0) {
                    if (remain > 2) { *p++ = L'\n'; *p++ = L' '; remain -= 2; }
                } else {
                    *p++ = L' '; *p++ = L','; *p++ = L' ';
                    remain -= 3;
                }
            }
            if (remain > len + 1) {
                wcsncpy_s(p, remain, seg, len);
                p += len;
                remain -= len;
            }
        }
        *p = 0;
    }
}

static void PaHudFormat(size_t groups, size_t working, size_t idle,
                        const wchar_t* itemsText) {
    swprintf(g_paHudText, sizeof(g_paHudText) / sizeof(g_paHudText[0]),
             L"组:%zu 工作:%zu 空闲:%zu", groups, working, idle);
    if (itemsText && itemsText[0]) {
        wcsncat_s(g_paHudText,
                  sizeof(g_paHudText) / sizeof(g_paHudText[0]),
                  L"\n产出:", 4);
        wcsncat_s(g_paHudText,
                  sizeof(g_paHudText) / sizeof(g_paHudText[0]),
                  itemsText, wcslen(itemsText));
    }
}

// ---- 窗口 ----
static LRESULT CALLBACK PaHudWndProc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam) {
    switch (msg) {
case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        // KB-111 (2026-11-01)：LWA 半透明窗口里逐笔 GDI 直绘（FillRect+多次
        //   DrawText）会被合成器逐笔呈现 → 经典闪烁。改为离屏双缓冲、一次
        //   BitBlt 提交；分配失败自动降级为直绘，不影响功能。
        HDC paHudMemDC = nullptr;
        HBITMAP paHudMemBmp = nullptr;
        HGDIOBJ paHudOldBmp = nullptr;
        HDC paHudSplash = dc;
        if (rc.right > 0 && rc.bottom > 0) {
            paHudMemDC = CreateCompatibleDC(dc);
            if (paHudMemDC) {
                paHudMemBmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
                if (paHudMemBmp) {
                    paHudOldBmp = SelectObject(paHudMemDC, paHudMemBmp);
                    dc = paHudMemDC;
                } else {
                    DeleteDC(paHudMemDC);
                    paHudMemDC = nullptr;
                }
            }
        }
        // 背景（旧版 RGB(22,25,30)）
        if (!g_paHudBg)
            g_paHudBg = CreateSolidBrush(RGB(22, 25, 30));
        FillRect(dc, &rc, g_paHudBg);
        // 左侧 accent 竖条（旧版 4px 绿 RGB(80,205,118)）
        RECT accent = rc;
        accent.right = accent.left + PaHudScale(4);
        if (!g_paHudAccent)
            g_paHudAccent = CreateSolidBrush(RGB(80, 205, 118));
        FillRect(dc, &accent, g_paHudAccent);
        if (g_paHudFont) {
            HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, g_paHudFont));
            SetBkMode(dc, TRANSPARENT);
            // 第一行（状态）：近白（旧版 RGB(246,247,249)）
            wchar_t line1[128];
            const wchar_t* newline = wcschr(g_paHudText, L'\n');
            if (newline) {
                const size_t len = static_cast<size_t>(newline - g_paHudText);
                wcsncpy_s(line1, sizeof(line1) / sizeof(line1[0]),
                          g_paHudText, len < 128 ? len : 127);
                line1[len < 128 ? len : 127] = 0;
            } else {
                wcsncpy_s(line1, sizeof(line1) / sizeof(line1[0]),
                          g_paHudText, _TRUNCATE);
            }
            RECT tr = rc;
            tr.left += PaHudScale(14);
            tr.top += PaHudScale(4);
            SetTextColor(dc, RGB(246, 247, 249));
            DrawTextW(dc, line1, -1, &tr, DT_LEFT | DT_TOP | DT_NOPREFIX);
            // 产出行（多行）：第一行从"产出:"开始，后续行缩进，全部亮蓝
            if (newline && newline[1] != 0) {
                int line = 0;
                const wchar_t* seg = newline + 1;
                while (*seg) {
                    const wchar_t* nl = wcschr(seg, L'\n');
                    const size_t segLen = nl ? static_cast<size_t>(nl - seg)
                                             : wcslen(seg);
                    RECT trn = rc;
                    trn.left += PaHudScale(14);
                    trn.top += PaHudScale(24 + line * 20);
                    SetTextColor(dc, RGB(110, 190, 255));
                    wchar_t lineBuf[128];
                    const size_t copyLen = segLen < 127 ? segLen : 127;
                    wcsncpy_s(lineBuf, sizeof(lineBuf) / sizeof(lineBuf[0]),
                              seg, copyLen);
                    lineBuf[copyLen] = 0;
                    DrawTextW(dc, lineBuf, -1, &trn,
                              DT_LEFT | DT_TOP | DT_NOPREFIX);
                    if (!nl) break;
                    seg = nl + 1;
                    ++line;
                    if (line > 4) break;
                }
            }
            SelectObject(dc, oldFont);
        }
        if (paHudMemDC && dc == paHudMemDC)
            BitBlt(paHudSplash, 0, 0, rc.right, rc.bottom, paHudMemDC, 0, 0, SRCCOPY);
        if (paHudMemDC) {
            if (paHudOldBmp) SelectObject(paHudMemDC, paHudOldBmp);
            if (paHudMemBmp) DeleteObject(paHudMemBmp);
            DeleteDC(paHudMemDC);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;  // 点击穿透
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static bool PaHudInitWindow() {
    if (g_paHudWindow) return true;
    WNDCLASSW wc = {};
    wc.lpfnWndProc = PaHudWndProc;
    wc.hInstance = g_module;
    wc.lpszClassName = PA_HUD_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log("[PA][HUD] RegisterClass failed err=%lu\n", GetLastError());
        return false;
    }
    const int w = PaHudScale(280);
    const int h = PaHudScale(56);
    g_paHudWindow = CreateWindowExW(
        // 与 ModManager 菜单一致（无 WS_EX_TRANSPARENT——会导致 layered 绘制跳过；
        // 点击穿透由 WM_NCHITTEST=HTTRANSPARENT 实现）
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED |
            WS_EX_NOACTIVATE,
        PA_HUD_CLASS, L"", WS_POPUP, 0, 0, w, h, nullptr, nullptr,
        g_module, nullptr);
    if (!g_paHudWindow) {
        Log("[PA] HUD window create failed err=%lu\n", GetLastError());
        return false;
    }
    SetLayeredWindowAttributes(g_paHudWindow, RGB(0, 0, 0), 200,
                              LWA_ALPHA);
    g_paHudFont = reinterpret_cast<HFONT>(CreateFontW(
        -PaHudScale(16), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei UI"));
    g_paHudBg = CreateSolidBrush(RGB(24, 24, 32));
    g_paHudAccent = CreateSolidBrush(RGB(90, 140, 220));
    return true;
}

static void PaHudPosition() {
    if (!g_paHudWindow) return;
    HWND game = QolFindGameWindow();
    if (!game) {
        Log("[PA] HUD pos: game window NOT FOUND\n");
        return;
    }
    RECT gr = {};
    if (!GetWindowRect(game, &gr)) {
        Log("[PA] HUD pos: GetWindowRect failed\n");
        return;
    }
    // 自适应尺寸：用 DT_CALCRECT 测量文本真实宽高（旧版 MeasureSize 语义）
    int textW = 0, textH = 0;
    if (g_paHudFont) {
        HDC mdc = GetDC(g_paHudWindow);
        if (mdc) {
            HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(mdc, g_paHudFont));
            RECT mr = {0, 0, 0, 0};
            DrawTextW(mdc, g_paHudText, -1, &mr,
                      DT_CALCRECT | DT_LEFT | DT_NOPREFIX);
            textW = mr.right - mr.left;
            textH = mr.bottom - mr.top;
            SelectObject(mdc, oldFont);
            ReleaseDC(g_paHudWindow, mdc);
        }
    }
    const int w = (textW > 0 ? textW : PaHudScale(240)) + PaHudScale(24);
    const int h = (textH > 0 ? textH : PaHudScale(24)) + PaHudScale(10);
    // v1.6 修正：已可见时不强制 SWP_SHOWWINDOW（每 2s 刷新重显会触发一次重绘闪烁）；
    //   仅当窗口尚未可见时才带 SWP_SHOWWINDOW 显示。
    const bool paHudWasVisible = IsWindowVisible(g_paHudWindow);
    SetWindowPos(g_paHudWindow, HWND_TOPMOST,
                 gr.left + PaHudScale(12),  // 左下角
                 gr.bottom - h - PaHudScale(12),
                 w, h,
                 SWP_NOACTIVATE | (paHudWasVisible ? 0 : SWP_SHOWWINDOW));
}

static void PaHudRefresh() {
    if (!g_paHudVisible || !g_paHudWindow) return;
    size_t groups = 0, working = 0, idle = 0;
    wchar_t items[256] = {};
    PaHudCollect(&groups, &working, &idle, items,
                 sizeof(items) / sizeof(items[0]));
    PaHudFormat(groups, working, idle, items);
    PaHudPosition();
    InvalidateRect(g_paHudWindow, nullptr, TRUE);
    UpdateWindow(g_paHudWindow);  // 同步强制重绘（AutoPet L822 同款，不依赖游戏 pump）
}

static void PaHudShow() {
    if (!PaHudInitWindow()) return;
    // 清除守卫残留状态 + 强制恢复 alpha（切换窗口失焦后 F5 恢复显示）
    QolHudMarkHiddenByMod(g_paHudWindow);
    SetLayeredWindowAttributes(g_paHudWindow, RGB(0, 0, 0), 200, LWA_ALPHA);
    PaHudPosition();
    g_paHudVisible = true;
    PaHudRefresh();
    Log("[PA] HUD shown\n");
}

static void PaHudHide() {
    if (g_paHudWindow && IsWindowVisible(g_paHudWindow))
        ShowWindow(g_paHudWindow, SW_HIDE);
    g_paHudVisible = false;
}

static void PaHudToggle() {
    if (g_paHudVisible) PaHudHide();
    else PaHudShow();
}

// F5 热键轮询（mod_tick 调用）
static void PaHudPoll() {
    // 消息泵：处理 HUD 窗口的 WM_PAINT 等（AutoPet PetHudPump L826-830 同款）
    if (g_paHudWindow) {
        if (!QolGameInForeground()) {
            QolHudGuardVisibility(g_paHudWindow);
        } else {
            MSG msg = {};
            while (PeekMessageW(&msg, g_paHudWindow, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    QolHotkeyCheckReload(&g_paHudHotkeys);
    const WORD toggleVk = QolHotKeysVk(&g_paHudHotkeys, 0);
    if (toggleVk == VK_UNDEFINED || toggleVk == 0) return;
    const bool down = (GetAsyncKeyState(toggleVk) & 0x8000) != 0;
    if (g_paHudToggleArmed) {
        if (!down) g_paHudToggleArmed = false;
    } else if (down) {
        g_paHudToggleArmed = true;
        PaHudToggle();
    }
    // 可见时每 2s 刷新一次（扫描数据变化）
    if (g_paHudVisible) {
        static ULONGLONG s_lastRefresh = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - s_lastRefresh >= 2000) {
            s_lastRefresh = now;
            PaHudRefresh();
        }
    }
}

static void PaHudInit() {
    QolHotKeysInit(&g_paHudHotkeys, "productionauto");
    if (g_paHudHotkeys.count == 0)
        QolHotKeysSetDefault(&g_paHudHotkeys, "F5");
    Log("[PA] HUD hotkey: %hs\n",
        g_paHudHotkeys.count > 0 ? g_paHudHotkeys.raw[0] : "none");
}

static void PaHudCleanup() {
    if (g_paHudWindow) {
        DestroyWindow(g_paHudWindow);
        g_paHudWindow = nullptr;
    }
    if (g_paHudFont) { DeleteObject(g_paHudFont); g_paHudFont = nullptr; }
    if (g_paHudBg) { DeleteObject(g_paHudBg); g_paHudBg = nullptr; }
    if (g_paHudAccent) { DeleteObject(g_paHudAccent); g_paHudAccent = nullptr; }
    g_paHudVisible = false;
}