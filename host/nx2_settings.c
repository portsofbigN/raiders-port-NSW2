// Game settings and the launcher shown before the game starts.
//
// The settings live in splatoon.ini next to the executable ("key = value" lines). At start they become the NX2_*
// environment variables the runtime reads (a variable already set in the environment wins, so tests and command lines
// keep working). The launcher is a small window that edits them; it is skipped when "launcher = 0" (unless Shift is
// held at start), with NX2_NO_LAUNCHER, and in automated runs (input scripts, screenshots).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <shlobj.h>

#include "host.h"

typedef struct {
    const char *key;            // ini key
    const char *env;            // environment variable it sets
    const char *def;            // default value
    const char *label[2];       // launcher label: Portuguese, English
    const char *const *values;  // choices
    const char *const *names[2];   // their labels: Portuguese, English
    int nvalues;
    int cat;                    // launcher category (CAT_*)
    const char *desc[2];        // what it does, shown under the options
    char value[64];
} Setting;

static int lang;   // launcher (and overlay) language: 0 Portuguese, 1 English
#define T(pt, en) (lang ? (en) : (pt))

enum { CAT_DISPLAY, CAT_IMAGE, CAT_PERF, CAT_PAD, CAT_GENERAL, NCAT };
static const char *const cat_names[2][NCAT] = {{"Tela", "Imagem", "Desempenho", "Controles", "Geral"},
                                               {"Display", "Image", "Performance", "Controls", "General"}};
static const char *const cat_titles[2][NCAT] = {{"TELA", "IMAGEM", "DESEMPENHO", "CONTROLES", "GERAL"},
                                                {"DISPLAY", "IMAGE", "PERFORMANCE", "CONTROLS", "GENERAL"}};

static const char *const v_size[] = {"1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"};
static const char *const n_size[] = {"1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
static const char *const v_full[] = {"0", "1"};
static const char *const n_full[] = {"Janela", "Tela cheia (sem bordas)"};
static const char *const e_full[] = {"Windowed", "Fullscreen (borderless)"};
static const char *const v_fps[] = {"30", "60", "120", "0"};
static const char *const n_fps[] = {"30 FPS", "60 FPS (recomendado)", "120 FPS (experimental)", "Sem limite"};
static const char *const e_fps[] = {"30 FPS", "60 FPS (recommended)", "120 FPS (experimental)", "Unlimited"};
static const char *const v_onoff[] = {"0", "1"};
static const char *const n_vsync[] = {"Desligado (menor latência)", "Ligado"};
static const char *const e_vsync[] = {"Off (lower latency)", "On"};
static const char *const v_overlay[] = {"off", "fps", "full"};
static const char *const n_overlay[] = {"Desligado", "Só FPS", "Completo (FPS, tempos, memória)"};
static const char *const e_overlay[] = {"Off", "FPS only", "Full (FPS, timings, memory)"};
static const char *const v_pad[] = {"auto", "keyboard"};
static const char *const n_pad[] = {"Automático (Xbox, PlayStation)", "Só teclado"};
static const char *const e_pad[] = {"Automatic (Xbox, PlayStation)", "Keyboard only"};
static const char *const v_rumble[] = {"0", "50", "100"};
static const char *const n_rumble[] = {"Desligada", "Fraca", "Forte"};
static const char *const e_rumble[] = {"Off", "Weak", "Strong"};
static const char *const v_motion[] = {"1", "0"};
static const char *const n_motion[] = {"Ligada (DualSense / DualShock 4)", "Desligada"};
static const char *const e_motion[] = {"On (DualSense / DualShock 4)", "Off"};
static const char *const v_gyro[] = {"0", "50", "100", "150"};
static const char *const n_gyro[] = {"Desligada", "Baixa", "Média", "Alta"};
static const char *const e_gyro[] = {"Off", "Low", "Medium", "High"};
static const char *const n_launch[] = {"Não", "Sim"};
static const char *const e_launch[] = {"No", "Yes"};
static const char *const v_mode[] = {"docked", "handheld"};
static const char *const n_mode[] = {"TV (saída 4K, mais bonito)", "Portátil (mais leve)"};
static const char *const e_mode[] = {"TV (4K output, best looking)", "Handheld (lighter)"};
static const char *const v_scaler[] = {"fsr", "nis", "linear"};
static const char *const n_scaler[] = {"AMD FSR 1", "NVIDIA Image Scaling (NIS)", "Linear (mais suave)"};
static const char *const e_scaler[] = {"AMD FSR 1", "NVIDIA Image Scaling (NIS)", "Linear (softer)"};
static const char *const v_sharp[] = {"0", "50", "80", "100"};
static const char *const n_sharp[] = {"Sem nitidez extra", "Média", "Alta (recomendado)", "Máxima"};
static const char *const e_sharp[] = {"No extra sharpening", "Medium", "High (recommended)", "Maximum"};
static const char *const v_layout[] = {"nintendo", "xbox"};
static const char *const n_layout[] = {"Posição do Switch (A à direita)", "Xbox / PlayStation (A embaixo)"};
static const char *const e_layout[] = {"Switch positions (A on the right)", "Xbox / PlayStation (A at the bottom)"};
static const char *const n_async[] = {"Esperar (sem falhas visuais)", "Em segundo plano (sem travadas)"};
static const char *const e_async[] = {"Wait (no visual gaps)", "In the background (no stutter)"};
static const char *const v_lang[] = {"auto", "pt", "en"};
static const char *const n_lang[] = {"Automático (idioma do Windows)", "Português", "English"};
static const char *const e_lang[] = {"Automatic (Windows language)", "Português", "English"};

static Setting settings[] = {
    {"window_size", "NX2_WINDOW_SIZE", "1280x720", {"Resolução da janela", "Window size"}, v_size, {n_size, n_size}, 5,
     CAT_DISPLAY,
     {"Tamanho da janela. O tamanho do monitor abre em tela cheia; maior que a tela, a janela se ajusta.",
      "Window size. The monitor's own size opens fullscreen; larger than the screen, the window shrinks to fit."}},
    {"fullscreen", "NX2_FULLSCREEN", "0", {"Modo de exibição", "Display mode"}, v_full, {n_full, e_full}, 2, CAT_DISPLAY,
     {"Janela ou tela cheia sem bordas. Durante o jogo: F11 ou Alt+Enter alterna.",
      "Windowed or borderless fullscreen. In game, F11 or Alt+Enter switches."}},
    {"mode", "NX2_OPERATION_MODE", "docked", {"Modo do console", "Console mode"}, v_mode, {n_mode, e_mode}, 2, CAT_DISPLAY,
     {"TV: o jogo gera imagem 4K (mais detalhe). Portátil: 1280x720, mais leve para PCs modestos.",
      "TV: the game renders a 4K picture (more detail). Handheld: 1280x720, lighter for modest PCs."}},
    {"upscaler", "NX2_UPSCALER", "fsr", {"Escala da imagem", "Image scaling"}, v_scaler, {n_scaler, e_scaler}, 3, CAT_IMAGE,
     {"Como a imagem é ampliada: AMD FSR 1 ou NVIDIA Image Scaling (mais nítidos) ou linear (mais suave).",
      "How the picture is upscaled: AMD FSR 1 or NVIDIA Image Scaling (sharper) or linear (softer)."}},
    {"sharpness", "NX2_SHARPNESS", "80", {"Nitidez", "Sharpness"}, v_sharp, {n_sharp, e_sharp}, 4, CAT_IMAGE,
     {"Quanto o FSR / NIS realçam os contornos depois de ampliar.",
      "How much FSR / NIS sharpen edges after upscaling."}},
    {"fps", "NX2_FPS", "60", {"Limite de FPS", "Frame rate limit"}, v_fps, {n_fps, e_fps}, 4, CAT_PERF,
     {"Quadros por segundo. 60 é o ritmo do console; acima disso é experimental.",
      "Frames per second. 60 is the console's pace; above that is experimental."}},
    {"vsync", "NX2_VSYNC", "0", {"VSync", "VSync"}, v_onoff, {n_vsync, e_vsync}, 2, CAT_PERF,
     {"Ligado evita cortes na imagem (tearing); desligado tem menos atraso.",
      "On avoids tearing; off has less input lag."}},
    {"overlay", "NX2_OVERLAY", "fps", {"Overlay de desempenho", "Performance overlay"}, v_overlay, {n_overlay, e_overlay},
     3, CAT_PERF,
     {"Mostra FPS e tempos no canto da tela. Durante o jogo, F1 alterna.",
      "Shows FPS and timings in a corner of the screen. In game, F1 cycles it."}},
    {"controller", "NX2_CONTROLLER", "auto", {"Controle", "Controller"}, v_pad, {n_pad, e_pad}, 2, CAT_PAD,
     {"Xbox (XInput), DualShock 4 e DualSense são detectados sozinhos, também com o jogo aberto.",
      "Xbox (XInput), DualShock 4 and DualSense are detected automatically, also while the game runs."}},
    {"layout", "NX2_PAD_LAYOUT", "nintendo", {"Botões", "Buttons"}, v_layout, {n_layout, e_layout}, 2, CAT_PAD,
     {"Switch: A fica no botão da direita. Xbox / PlayStation: A no botão de baixo.",
      "Switch: A is the right button. Xbox / PlayStation: A is the bottom button."}},
    {"rumble", "NX2_RUMBLE", "100", {"Vibração", "Rumble"}, v_rumble, {n_rumble, e_rumble}, 3, CAT_PAD,
     {"Força da vibração do controle (Xbox e PlayStation, com fio ou Bluetooth).",
      "Controller rumble strength (Xbox and PlayStation, wired or Bluetooth)."}},
    {"motion", "NX2_GYRO", "1", {"Mira por movimento", "Motion aiming"}, v_motion, {n_motion, e_motion}, 2, CAT_PAD,
     {"Como no Switch: mova o controle para mirar (giroscópio do DualSense / DualShock 4). Outros controles usam o analógico R.",
      "As on the Switch: move the controller to aim (DualSense / DualShock 4 gyro). Other controllers use the right stick."}},
    {"aim_stick", "NX2_GYRO_STICK", "100", {"Mira vertical (analógico R)", "Vertical aim (right stick)"}, v_gyro,
     {n_gyro, e_gyro}, 4, CAT_PAD,
     {"O jogo mira para cima e para baixo com o movimento do controle; o analógico R faz esse papel. Desligue se tirar os controles de movimento no jogo.",
      "The game aims up and down with controller motion; the right stick does that here. Turn it off if you disable motion controls in the game."}},
    {"game_dir", "NX2_GAME_DIR", "", {"Pasta do jogo", "Game folder"}, NULL, {NULL, NULL}, 0, CAT_GENERAL,
     {"Pasta com as subpastas exefs e romfs do jogo (atualização 65536). Clique para escolher.",
      "Folder with the game's exefs and romfs subfolders (update 65536). Click to choose."}},
    {"language", NULL, "auto", {"Idioma", "Language"}, v_lang, {n_lang, e_lang}, 3, CAT_GENERAL,
     {"Idioma deste menu e do overlay. Automático segue o idioma do Windows.",
      "Language of this menu and of the overlay. Automatic follows Windows' language."}},
    {"launcher", NULL, "1", {"Mostrar este menu ao iniciar", "Show this menu at start"}, v_onoff, {n_launch, e_launch}, 2,
     CAT_GENERAL,
     {"Com \"Não\", o jogo abre direto. Segure Shift ao abrir para ver este menu de novo.",
      "With \"No\" the game starts directly. Hold Shift when opening it to see this menu again."}},
};
#define NSET ((int)(sizeof settings / sizeof *settings))

static void resolve_lang(const char *v) {   // "auto": Portuguese when Windows is in Portuguese, else English
    if (!strcmp(v, "pt")) lang = 0;
    else if (!strcmp(v, "en")) lang = 1;
    else lang = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE ? 0 : 1;
}

static char ini_path[MAX_PATH];

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static Setting *find(const char *key) {
    for (int i = 0; i < NSET; i++)
        if (!_stricmp(settings[i].key, key)) return &settings[i];
    return NULL;
}

static void load(void) {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *slash = strrchr(exe, '\\');
    if (slash) slash[1] = 0;
    snprintf(ini_path, sizeof ini_path, "%ssplatoon.ini", exe);
    for (int i = 0; i < NSET; i++) snprintf(settings[i].value, sizeof settings[i].value, "%s", settings[i].def);
    FILE *f = fopen(ini_path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k), trim(v);
        Setting *s = find(k);
        if (s && *v) snprintf(s->value, sizeof s->value, "%s", v);
    }
    fclose(f);
}

static void save(void) {
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.new", ini_path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# Splatoon Raiders (recomp): configurações do jogo, editadas pelo menu inicial.\n"
               "# Segure Shift ao abrir o jogo para ver o menu mesmo com launcher = 0.\n");
    for (int i = 0; i < NSET; i++) fprintf(f, "%s = %s\n", settings[i].key, settings[i].value);
    fclose(f);
    MoveFileExA(tmp, ini_path, MOVEFILE_REPLACE_EXISTING);   // replaced only once the new file is complete
}

static void apply(void) {
    for (int i = 0; i < NSET; i++) {
        Setting *s = &settings[i];
        if (!s->env || getenv(s->env)) continue;   // the environment wins
        if (!strcmp(s->env, "NX2_VSYNC") && strcmp(s->value, "1")) continue;   // NX2_VSYNC is read as present / absent
        if (!strcmp(s->env, "NX2_CONTROLLER") && !strcmp(s->value, "auto")) continue;
        if (!s->value[0]) continue;   // the game folder left at the build's default
        _putenv_s(s->env, s->value);
    }
}

// ---- launcher window: Splatoon Raiders colours (dark teal panels, orange / turquoise / yellow ink), categories in a
// sidebar, a description of the option under the mouse. Painted with GDI into a back buffer; the option lists are
// combo boxes in the system's dark style.
enum { ID_PLAY = 100, ID_QUIT = 101, ID_COMBO = 200 };
#define LW 940
#define LH 620
#define C_BG RGB(16, 36, 34)
#define C_PANEL RGB(28, 56, 52)
#define C_PANEL_EDGE RGB(52, 98, 90)
#define C_ORANGE RGB(242, 107, 33)
#define C_TEAL RGB(30, 214, 190)
#define C_YELLOW RGB(236, 226, 44)
#define C_TEXT RGB(240, 242, 238)
#define C_DIM RGB(160, 184, 178)
#define C_DARK RGB(14, 26, 24)
static HWND combos[NSET], win_l;
static int launch_result, cur_cat, hover_cat = -1, hover_set = -1;
static HFONT f_title, f_title2, f_cat, f_label, f_desc, f_btn, f_small;
static HBRUSH br_bg, br_panel, br_list;
static RECT r_side[NCAT], r_rows[NSET], r_panel, r_desc;

static void game_dir_shown(char *out, size_t n) {   // the chosen folder, or the build's default
    const char *v = find("game_dir")->value;
    if (*v) {
        snprintf(out, n, "%s", v);
        return;
    }
    snprintf(out, n, "%s", rt_default_exefs);
    char *e = strrchr(out, '/');
    if (e && !strcmp(e, "/exefs")) *e = 0;
}

static void fill_combo(int i) {   // option names in the current language, keeping the selection
    Setting *s = &settings[i];
    if (!s->nvalues) {   // a folder: the button shows it
        InvalidateRect(combos[i], NULL, FALSE);
        return;
    }
    HWND cb = combos[i];
    int sel = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    if (sel < 0)
        for (int k = 0; k < s->nvalues; k++)
            if (!strcmp(s->values[k], s->value)) sel = k;
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (int k = 0; k < s->nvalues; k++) {
        wchar_t wn[96];
        MultiByteToWideChar(CP_UTF8, 0, s->names[lang][k], -1, wn, 96);
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)wn);
    }
    SendMessageW(cb, CB_SETCURSEL, (WPARAM)(sel < 0 ? 0 : sel), 0);
}

static void read_combos(void) {
    for (int i = 0; i < NSET; i++) {
        if (!settings[i].nvalues) continue;
        int sel = (int)SendMessageA(combos[i], CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < settings[i].nvalues)
            snprintf(settings[i].value, sizeof settings[i].value, "%s", settings[i].values[sel]);
    }
}

static void wtext(HDC dc, HFONT f, COLORREF c, int x, int y, const char *s) {   // UTF-8 text at x, y
    wchar_t w[256];
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 256);
    SelectObject(dc, f);
    SetTextColor(dc, c);
    TextOutW(dc, x, y, w, (int)wcslen(w));
}

static void fill_poly(HDC dc, COLORREF c, const POINT *p, int n) {
    HBRUSH b = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    Polygon(dc, p, n);
    SelectObject(dc, ob), SelectObject(dc, op);
    DeleteObject(b), DeleteObject(pen);
}

static void skew_box(HDC dc, COLORREF c, int x, int y, int w, int h, int k) {   // a slanted tag, as in the game's UI
    POINT p[4] = {{x + k, y}, {x + w, y}, {x + w - k, y + h}, {x, y + h}};
    fill_poly(dc, c, p, 4);
}

static void blob(HDC dc, COLORREF c, int cx, int cy, int r, unsigned seed) {   // an ink splat: a drop and droplets
    HBRUSH b = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    for (int i = 0; i < 9; i++) {
        seed = seed * 1103515245u + 12345u;
        double a = (seed >> 8 & 1023) / 1024.0 * 6.2832;
        seed = seed * 1103515245u + 12345u;
        double d = r * (0.85 + (seed >> 8 & 255) / 255.0 * 0.6);
        int rr = r / 5 + (int)(seed >> 16 & 15) * r / 60;
        int x = cx + (int)(cos(a) * d), y = cy + (int)(sin(a) * d);
        Ellipse(dc, x - rr, y - rr, x + rr, y + rr);
        int x2 = cx + (int)(cos(a) * (d + rr * 1.8)), y2 = cy + (int)(sin(a) * (d + rr * 1.8));
        Ellipse(dc, x2 - rr / 2, y2 - rr / 2, x2 + rr / 2, y2 + rr / 2);
    }
    SelectObject(dc, ob), SelectObject(dc, op);
    DeleteObject(b), DeleteObject(pen);
}

static void layout_rects(void) {
    for (int i = 0; i < NCAT; i++) r_side[i] = (RECT){24, 136 + i * 54, 224, 136 + i * 54 + 44};
    r_panel = (RECT){246, 120, LW - 24, LH - 96};
    r_desc = (RECT){r_panel.left + 28, r_panel.bottom - 74, r_panel.right - 28, r_panel.bottom - 14};
    int row[NCAT] = {0}, cnt[NCAT] = {0};
    for (int i = 0; i < NSET; i++) cnt[settings[i].cat]++;
    for (int i = 0; i < NSET; i++) {
        int c = settings[i].cat, k = row[c]++;
        int sp = 60;   // more than four rows: closer together, above the description
        if (cnt[c] > 4) sp = (r_desc.top - 20 - (r_panel.top + 74)) / cnt[c];
        int y = r_panel.top + 74 + k * sp;
        r_rows[i] = (RECT){r_panel.left + 20, y - 8, r_panel.right - 20, y + (sp < 52 ? sp - 8 : 44)};
    }
}

static void show_category(int c) {
    cur_cat = c;
    for (int i = 0; i < NSET; i++) ShowWindow(combos[i], settings[i].cat == c ? SW_SHOW : SW_HIDE);
    hover_set = -1;
    InvalidateRect(win_l, NULL, FALSE);
}

static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(h, &ps);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bm = CreateCompatibleBitmap(wdc, LW, LH);
    HGDIOBJ obm = SelectObject(dc, bm);
    SetBkMode(dc, TRANSPARENT);
    RECT all = {0, 0, LW, LH};
    FillRect(dc, &all, br_bg);
    // ink splats around the edges
    blob(dc, RGB(150, 66, 22), LW + 10, -20, 120, 7);
    blob(dc, C_ORANGE, LW - 40, 30, 46, 11);
    blob(dc, RGB(18, 120, 108), -70, LH + 40, 130, 3);
    blob(dc, C_TEAL, 30, LH - 20, 22, 5);
    blob(dc, RGB(140, 132, 20), LW - 150, LH - 20, 40, 9);
    blob(dc, RGB(24, 92, 84), 420, -30, 60, 13);
    // title
    wtext(dc, f_title2, C_YELLOW, 34, 16, "SPLATOON");
    wtext(dc, f_title, C_TEXT, 28, 38, "RAIDERS");
    wtext(dc, f_small, C_DIM, 300, 78, T("Recompilação para PC   •   Configurações", "PC recompilation   •   Settings"));
    // sidebar
    for (int i = 0; i < NCAT; i++) {
        RECT r = r_side[i];
        if (i == cur_cat) skew_box(dc, C_ORANGE, r.left, r.top, r.right - r.left, r.bottom - r.top, 12);
        else if (i == hover_cat) skew_box(dc, RGB(34, 70, 64), r.left, r.top, r.right - r.left, r.bottom - r.top, 12);
        wtext(dc, f_cat, i == cur_cat ? RGB(255, 255, 255) : C_DIM, r.left + 26, r.top + 8, cat_names[lang][i]);
    }
    // panel
    HPEN pen = CreatePen(PS_SOLID, 2, C_PANEL_EDGE);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br_panel);
    RoundRect(dc, r_panel.left, r_panel.top, r_panel.right, r_panel.bottom, 22, 22);
    SelectObject(dc, op), SelectObject(dc, ob);
    DeleteObject(pen);
    skew_box(dc, C_YELLOW, r_panel.left + 20, r_panel.top - 14, 170, 34, 10);
    wtext(dc, f_cat, C_DARK, r_panel.left + 38, r_panel.top - 10, cat_titles[lang][cur_cat]);
    for (int i = 0; i < NSET; i++) {
        if (settings[i].cat != cur_cat) continue;
        RECT r = r_rows[i];
        if (i == hover_set) {
            HBRUSH hb = CreateSolidBrush(RGB(36, 72, 66));
            HGDIOBJ o2 = SelectObject(dc, hb), o3 = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, r.left, r.top, r.right, r.bottom, 14, 14);
            SelectObject(dc, o2), SelectObject(dc, o3);
            DeleteObject(hb);
        }
        wtext(dc, f_label, C_TEXT, r.left + 14, r.top + 13, settings[i].label[lang]);
    }
    // description of the option under the mouse (or the category's first)
    int d = hover_set;
    if (d < 0)
        for (int i = 0; i < NSET && d < 0; i++)
            if (settings[i].cat == cur_cat) d = i;
    if (d >= 0) {
        HPEN lp = CreatePen(PS_SOLID, 1, C_PANEL_EDGE);
        HGDIOBJ o4 = SelectObject(dc, lp);
        MoveToEx(dc, r_desc.left, r_desc.top - 10, NULL);
        LineTo(dc, r_desc.right, r_desc.top - 10);
        SelectObject(dc, o4);
        DeleteObject(lp);
        wchar_t w[512];
        MultiByteToWideChar(CP_UTF8, 0, settings[d].desc[lang], -1, w, 512);
        SelectObject(dc, f_desc);
        SetTextColor(dc, C_DIM);
        RECT rd = r_desc;
        DrawTextW(dc, w, -1, &rd, DT_WORDBREAK | DT_LEFT | DT_TOP);
    }
    // footer
    wtext(dc, f_small, RGB(120, 150, 144), 250, LH - 40, T("F1: overlay  •  F11: tela cheia", "F1: overlay  •  F11: fullscreen"));
    wtext(dc, f_small, RGB(120, 150, 144), 250, LH - 22, T("Shift ao abrir: este menu", "Shift at start: this menu"));
    BitBlt(wdc, 0, 0, LW, LH, dc, 0, 0, SRCCOPY);
    SelectObject(dc, obm);
    DeleteObject(bm);
    DeleteDC(dc);
    EndPaint(h, &ps);
}

static void draw_combo_item(const DRAWITEMSTRUCT *di) {   // option lists in the launcher's colours
    if (di->itemID == (UINT)-1) return;
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    int edit = (di->itemState & ODS_COMBOBOXEDIT) != 0, sel = (di->itemState & ODS_SELECTED) != 0;
    COLORREF bg = edit ? RGB(24, 46, 43) : sel ? C_TEAL : RGB(20, 40, 38);
    HBRUSH b = CreateSolidBrush(bg);
    FillRect(dc, &r, b);
    DeleteObject(b);
    wchar_t t[128] = L"";
    SendMessageW(di->hwndItem, CB_GETLBTEXT, di->itemID, (LPARAM)t);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, !edit && sel ? C_DARK : C_TEXT);
    r.left += 10;
    DrawTextW(dc, t, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void draw_button(const DRAWITEMSTRUCT *di) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    FillRect(dc, &r, br_bg);
    int play = di->CtlID == ID_PLAY, down = (di->itemState & ODS_SELECTED) != 0, focus = (di->itemState & ODS_FOCUS) != 0;
    int w = r.right - r.left, hh = r.bottom - r.top, o = down ? 2 : 0;
    if (play) {
        skew_box(dc, RGB(10, 120, 106), r.left + 4, r.top + 6, w - 4, hh - 6, 16);
        skew_box(dc, focus ? RGB(80, 240, 216) : C_TEAL, r.left + o, r.top + o, w - 6, hh - 6, 16);
    } else {
        skew_box(dc, RGB(60, 84, 80), r.left + o, r.top + o + 6, w - 4, hh - 12, 12);
    }
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, play ? f_btn : f_cat);
    SetTextColor(dc, play ? C_DARK : C_TEXT);
    RECT t = {r.left + o, r.top + o + (play ? 0 : 3), r.right - 6 + o, r.bottom - 6 + o + (play ? 0 : 3)};
    DrawTextW(dc, play ? (lang ? L"PLAY!" : L"JOGAR!") : (lang ? L"Quit" : L"Sair"), -1, &t,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// The game folder: exefs\main and a romfs folder in it. 1 when valid.
static int game_dir_ok(const wchar_t *d) {
    wchar_t p[MAX_PATH + 32];
    swprintf(p, MAX_PATH + 32, L"%ls/exefs/main", d);
    DWORD a = GetFileAttributesW(p);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    swprintf(p, MAX_PATH + 32, L"%ls/romfs", d);
    a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void pick_game_dir(HWND h, int i) {
    BROWSEINFOW bi = {0};
    bi.hwndOwner = h;
    bi.lpszTitle = lang ? L"Choose the game folder (with exefs and romfs inside)"
                        : L"Escolha a pasta do jogo (com exefs e romfs dentro)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
    if (!id) return;
    wchar_t d[MAX_PATH];
    int got = SHGetPathFromIDListW(id, d);
    CoTaskMemFree(id);
    if (!got) return;
    if (!game_dir_ok(d)) {
        MessageBoxW(h, lang ? L"This folder does not have exefs/main and romfs. Choose the folder that contains both "
                              L"(the game's update 65536, with the base romfs merged)."
                            : L"Esta pasta não tem exefs/main e romfs. Escolha a pasta que contém as duas "
                              L"(a atualização 65536 do jogo, com o romfs base mesclado).",
                    L"Splatoon Raiders", MB_ICONWARNING);
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, d, -1, settings[i].value, sizeof settings[i].value, NULL, NULL);
    for (char *c = settings[i].value; *c; c++)
        if (*c == 92) *c = '/';   // backslashes to slashes
    InvalidateRect(combos[i], NULL, FALSE);
}

static void draw_path(const DRAWITEMSTRUCT *di) {   // the game folder button: the path in a dark field
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    HBRUSH b = CreateSolidBrush((di->itemState & ODS_SELECTED) ? RGB(36, 72, 66) : RGB(24, 46, 43));
    FillRect(dc, &r, b);
    DeleteObject(b);
    HPEN pen = CreatePen(PS_SOLID, 1, (di->itemState & ODS_FOCUS) ? C_TEAL : C_PANEL_EDGE);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, op), SelectObject(dc, ob);
    DeleteObject(pen);
    char path[300];
    game_dir_shown(path, sizeof path);
    wchar_t w[300];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 300);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, f_desc);
    SetTextColor(dc, C_TEXT);
    RECT t = {r.left + 10, r.top, r.right - 8, r.bottom};
    DrawTextW(dc, w, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
}

static LRESULT CALLBACK launcher_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT:
        paint(h);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DRAWITEM:
        if (((const DRAWITEMSTRUCT *)l)->CtlType == ODT_COMBOBOX) draw_combo_item((const DRAWITEMSTRUCT *)l);
        else if (((const DRAWITEMSTRUCT *)l)->CtlID >= ID_COMBO) draw_path((const DRAWITEMSTRUCT *)l);
        else draw_button((const DRAWITEMSTRUCT *)l);
        return TRUE;
    case WM_MEASUREITEM:
        ((MEASUREITEMSTRUCT *)l)->itemHeight = 26;
        return TRUE;
    case WM_MOUSEMOVE: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        int hc = -1, hs = -1;
        for (int i = 0; i < NCAT; i++)
            if (PtInRect(&r_side[i], p)) hc = i;
        for (int i = 0; i < NSET; i++)
            if (settings[i].cat == cur_cat && PtInRect(&r_rows[i], p)) hs = i;
        if (hc != hover_cat || (hs >= 0 && hs != hover_set)) {
            hover_cat = hc;
            if (hs >= 0) hover_set = hs;
            InvalidateRect(h, NULL, FALSE);
        }
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
        TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE:
        hover_cat = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p = {(short)LOWORD(l), (short)HIWORD(l)};
        for (int i = 0; i < NCAT; i++)
            if (PtInRect(&r_side[i], p)) show_category(i);
        return 0;
    }
    case WM_KEYDOWN:   // Ctrl+Tab / Ctrl+Shift+Tab: next / previous category
        if (w == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000)) {
            show_category((cur_cat + ((GetKeyState(VK_SHIFT) & 0x8000) ? NCAT - 1 : 1)) % NCAT);
            return 0;
        }
        break;
    case DM_GETDEFID:
        return MAKELONG(ID_PLAY, DC_HASDEFID);
    case WM_COMMAND:
        if (LOWORD(w) == ID_PLAY || LOWORD(w) == IDOK) {
            read_combos();
            launch_result = 1;
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(w) == ID_QUIT || LOWORD(w) == IDCANCEL) {
            launch_result = 0;
            DestroyWindow(h);
            return 0;
        }
        if (HIWORD(w) == BN_CLICKED && LOWORD(w) >= ID_COMBO && LOWORD(w) - ID_COMBO < NSET &&
            !settings[LOWORD(w) - ID_COMBO].nvalues) {
            pick_game_dir(h, LOWORD(w) - ID_COMBO);
            return 0;
        }
        if (HIWORD(w) == CBN_SELCHANGE && LOWORD(w) - ID_COMBO >= 0 && LOWORD(w) - ID_COMBO < NSET &&
            !strcmp(settings[LOWORD(w) - ID_COMBO].key, "language")) {   // the language: every text again
            int i = LOWORD(w) - ID_COMBO, sel = (int)SendMessageW(combos[i], CB_GETCURSEL, 0, 0);
            if (sel >= 0) resolve_lang(settings[i].values[sel]);
            for (int k = 0; k < NSET; k++) fill_combo(k);
            InvalidateRect(h, NULL, FALSE);
            InvalidateRect(GetDlgItem(h, ID_PLAY), NULL, FALSE);
            InvalidateRect(GetDlgItem(h, ID_QUIT), NULL, FALSE);
        }
        if (HIWORD(w) == CBN_SETFOCUS || HIWORD(w) == CBN_DROPDOWN) {
            int i = LOWORD(w) - ID_COMBO;
            if (i >= 0 && i < NSET && i != hover_set) {
                hover_set = i;
                InvalidateRect(h, NULL, FALSE);
            }
        }
        break;
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w;
        SetTextColor(dc, C_TEXT);
        SetBkColor(dc, RGB(24, 46, 43));
        return (LRESULT)br_list;
    }
    case WM_CLOSE:
        launch_result = 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static HFONT make_font(int px, int weight, int italic, const wchar_t *face) {
    return CreateFontW(-px, 0, 0, 0, weight, (DWORD)italic, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}

static int run_launcher(void) {
    HINSTANCE inst = GetModuleHandleW(NULL);
    br_bg = CreateSolidBrush(C_BG), br_panel = CreateSolidBrush(C_PANEL), br_list = CreateSolidBrush(RGB(24, 46, 43));
    f_title = make_font(54, FW_BLACK, 1, L"Segoe UI Black");
    f_title2 = make_font(22, FW_BLACK, 1, L"Segoe UI Black");
    f_cat = make_font(19, FW_BLACK, 1, L"Segoe UI Black");
    f_label = make_font(17, FW_SEMIBOLD, 0, L"Segoe UI Semibold");
    f_desc = make_font(15, FW_NORMAL, 0, L"Segoe UI");
    f_btn = make_font(26, FW_BLACK, 1, L"Segoe UI Black");
    f_small = make_font(13, FW_NORMAL, 0, L"Segoe UI");
    HFONT f_combo = make_font(15, FW_NORMAL, 0, L"Segoe UI");
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = launcher_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SplatoonLauncher";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = br_bg;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);
    RECT r = {0, 0, LW, LH};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    AdjustWindowRect(&r, style, FALSE);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    win_l = CreateWindowExW(0, wc.lpszClassName, L"Splatoon Raiders (recomp)", style, (sw - ww) / 2, (sh - wh) / 2, ww,
                            wh, NULL, NULL, inst, NULL);
    {   // dark title bar (Windows 10 2004+ / 11)
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        typedef HRESULT(WINAPI * SetAttr)(HWND, DWORD, LPCVOID, DWORD);
        SetAttr set = dwm ? (SetAttr)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
        BOOL dark = TRUE;
        if (set) set(win_l, 20, &dark, sizeof dark);
    }
    typedef HRESULT(WINAPI * SetTheme)(HWND, LPCWSTR, LPCWSTR);
    HMODULE ux = LoadLibraryA("uxtheme.dll");
    SetTheme theme = ux ? (SetTheme)(void *)GetProcAddress(ux, "SetWindowTheme") : NULL;
    layout_rects();
    for (int i = 0; i < NSET; i++) {
        Setting *s = &settings[i];
        RECT rr = r_rows[i];
        HWND cb = !s->nvalues ? CreateWindowExW(0, L"BUTTON", NULL, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, rr.right - 360,
                                                rr.top + 9, 346, 28, win_l, (HMENU)(INT_PTR)(ID_COMBO + i), inst, NULL)
                              : CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL |
                                      CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                  rr.right - 360, rr.top + 9, 346, 300, win_l, (HMENU)(INT_PTR)(ID_COMBO + i), inst, NULL);
        if (theme) theme(cb, L"DarkMode_CFD", NULL);
        SendMessageW(cb, WM_SETFONT, (WPARAM)f_combo, TRUE);
        combos[i] = cb;
        (void)s;
        fill_combo(i);
    }
    HWND play = CreateWindowExW(0, L"BUTTON", L"JOGAR!", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, LW - 24 - 210,
                                LH - 82, 210, 62, win_l, (HMENU)ID_PLAY, inst, NULL);
    CreateWindowExW(0, L"BUTTON", L"Sair", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, LW - 24 - 210 - 130, LH - 76,
                    116, 52, win_l, (HMENU)ID_QUIT, inst, NULL);
    show_category(CAT_DISPLAY);
    ShowWindow(win_l, SW_SHOW);
    SetForegroundWindow(win_l);
    SetFocus(play);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(win_l, WM_KEYDOWN, msg.wParam, msg.lParam);
            continue;
        }
        if (IsDialogMessageW(win_l, &msg)) continue;   // Tab between controls, Enter plays, Esc quits
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnregisterClassW(wc.lpszClassName, inst);
    return launch_result;
}

// Called first thing in main (after the address space is claimed): settings, the launcher, then the environment.
// Returns 0 when the player closed the launcher (quit).
int nx2_settings_start(void) {
    load();
    resolve_lang(find("language")->value);
    int automated = getenv("NX2_INPUT_SCRIPT") || getenv("NX2_INPUT_SCRIPT_FILE") || getenv("NX2_SHOT") ||
                    getenv("NX2_SHOT_EVERY") || getenv("NX2_NO_LAUNCHER");
    Setting *ls = find("launcher");
    int show = !automated && (strcmp(ls->value, "0") || (GetAsyncKeyState(VK_SHIFT) & 0x8000));
    if (show) {
        if (!run_launcher()) return 0;
        save();
    } else if (GetFileAttributesA(ini_path) == INVALID_FILE_ATTRIBUTES && !automated) {
        save();   // a first file with the defaults, to edit by hand
    }
    resolve_lang(find("language")->value);
    if (!getenv("NX2_UI_LANG")) _putenv_s("NX2_UI_LANG", lang ? "en" : "pt");   // the overlay's language
    apply();
    return 1;
}

// The game folder (NX2_GAME_DIR, from the launcher: its exefs and romfs) and the saves ("profile" next to the
// program, so they move with it). 0 when the game's files are missing: a message says how to choose the folder.
int nx2_game_paths(const char **exefs, const char **romfs, const char **profile) {
    static char e[1024], r[1024], p[1024];
    const char *gd = getenv("NX2_GAME_DIR");
    if (gd && *gd) {
        snprintf(e, sizeof e, "%s/exefs", gd), snprintf(r, sizeof r, "%s/romfs", gd);
        *exefs = e, *romfs = r;
    }
    char exe[1024];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    char *slash = n ? strrchr(exe, 92) : NULL;
    if (slash) {
        *slash = 0;
        snprintf(p, sizeof p, "%s/profile", exe);
        for (char *c = p; *c; c++)
            if (*c == 92) *c = '/';
        *profile = p;
    }
    char main_path[1100];
    snprintf(main_path, sizeof main_path, "%s/main", *exefs);
    if (GetFileAttributesA(main_path) != INVALID_FILE_ATTRIBUTES) return 1;
    wchar_t msg[1400], wdir[1100];
    MultiByteToWideChar(CP_UTF8, 0, *exefs, -1, wdir, 1100);
    swprintf(msg, 1400,
             lang ? L"The game files were not found:\n%ls\n\nOpen the game holding Shift and choose the game folder "
                    L"(General > Game folder): the folder with exefs and romfs inside."
                  : L"Os arquivos do jogo não foram encontrados:\n%ls\n\nAbra o jogo segurando Shift e escolha a "
                    L"pasta do jogo (Geral > Pasta do jogo): a pasta com exefs e romfs dentro.",
             wdir);
    MessageBoxW(NULL, msg, L"Splatoon Raiders", MB_ICONERROR);
    return 0;
}
