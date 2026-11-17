/*
 * ROLEXWIN.CPP -- Reloj Rolex Datejust (Win32/Win64)
 * Todo en ROLEXWIN.H (geometria + datos). No hace falta .inc.
 *
 * cl /EHsc /O2 /DUNICODE /D_UNICODE /TP ROLEXWIN.CPP /link gdiplus.lib user32.lib gdi32.lib ole32.lib winmm.lib /SUBSYSTEM:WINDOWS
 * g++ -O2 -municode -mwindows ROLEXWIN.CPP -o ROLEXWIN.EXE -lgdiplus -luser32 -lgdi32 -lole32 -lwinmm
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <mmsystem.h>   /* PlaySoundA, SND_MEMORY para el tic-tac */
#include <gdiplus.h>
#include <cmath>
#include <cstring>
#include <cstdlib>      /* malloc, free, abs */
#include <algorithm>
#include <atomic>        /* std::atomic<bool> para g_tictac_run (cross-thread) */

#include "ROLEXWIN.H"

using namespace Gdiplus;

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")   /* PlaySoundA para el tic-tac */

static const Color C_OSC(255, 75, 75, 80);   /* filo oscuro del canto de la aguja */
static const Color C_MID(255, 160, 160, 165);   /* acero gris de los flancos */
static const Color C_CLA(255, 205, 205, 210);   /* faceta iluminada */
static const Color C_HI(255, 240, 240, 243);
static const Color C_NEG(255, 5, 5, 5);
static const Color C_LUME(255, 252, 252, 248);   /* lume Chromalight, blanco puro */
static const Color C_ARO1(255, 220, 220, 225);
static const Color C_ARO2(255, 60, 60, 65);
static const Color C_LUPA(255, 255, 255, 255);

static HWND      g_hwnd = NULL;
static Bitmap* g_bg = NULL;
static Bitmap* g_back = NULL;
static Bitmap* g_limpia = NULL;   /* fondo con la fecha cocinada, SIN agujas:
                                      la lupa muestrea de aqui (igual que la
                                      esfera limpia) */
static ULONG_PTR g_token = 0;
static HICON     g_hIcon = NULL;
static HICON     g_hIconSm = NULL;   /* icono 16x16 para la barra de tareas */

static unsigned  g_ah = 0xFFFF, g_am = 0xFFFF, g_as = 0xFFFF;
static int       g_day = -1;
static int       g_day_limpia = -2;   /* dia cocinado en g_limpia */
static bool      g_needRedraw = true;
static bool      g_fullscreen = false;
static LONG_PTR  g_oldStyle = 0;
static RECT      g_winRect = { 0 };

/* Brushes y Pens cacheados: se crean una sola vez al arrancar para no
   asignar nada en cada fotograma (antes el lume construia un SolidBrush
   en cada llamada a dibujar_baston, dos por frame). */
static SolidBrush* g_brOsc, * g_brMid, * g_brCla, * g_brHi, * g_brNeg,
* g_brLume, * g_brLupa, * g_brBlack;
static Pen* g_penAro1, * g_penAro2, * g_penHi;
static Pen* g_penSBody = NULL;
static Pen* g_penSLit = NULL;

static void sincos256(unsigned a, float* s, float* c)
{
    float r = (float)a * (6.28318530718f / 256.0f) - 1.57079632679f;
    *s = sinf(r); *c = cosf(r);
}

static void angulos_ahora(unsigned* ah, unsigned* am, unsigned* as, int* day)
{
    SYSTEMTIME st; GetLocalTime(&st);
    *day = st.wDay;
    double sec = st.wHour * 3600.0 + st.wMinute * 60.0 + st.wSecond + st.wMilliseconds / 1000.0;
    unsigned long vs = (unsigned long)(sec * 256.0 / 60.0);
    unsigned long vm = vs / 60;
    *as = (unsigned)(vs & 255u);
    *am = (unsigned)(vm & 255u);
    *ah = (unsigned)((vm / 12u) & 255u);
}

static Bitmap* LoadBitmapFromMemory(const unsigned char* data, unsigned size)
{
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hMem) return NULL;
    void* p = GlobalLock(hMem); if (!p) { GlobalFree(hMem); return NULL; }
    memcpy(p, data, size); GlobalUnlock(hMem);
    IStream* st = NULL;
    if (CreateStreamOnHGlobal(hMem, TRUE, &st) != S_OK) { GlobalFree(hMem); return NULL; }
    Bitmap* bmp = new Bitmap(st); st->Release();
    if (!bmp || bmp->GetLastStatus() != Ok) { delete bmp; return NULL; }
    return bmp;
}

/* Carga icono desde .ICO embebido (LookupIconIdFromDirectoryEx). */
static HICON LoadIconFromMemory(const unsigned char* data, unsigned size, int sz)
{
    int alt = (sz == 32) ? 16 : 32;
    int id = LookupIconIdFromDirectoryEx((PBYTE)data, TRUE, sz, sz, LR_DEFAULTCOLOR);
    if (id <= 0)
        id = LookupIconIdFromDirectoryEx((PBYTE)data, TRUE, alt, alt, LR_DEFAULTCOLOR);
    if (id <= 0) return NULL;
    HICON h = CreateIconFromResourceEx((PBYTE)data + id, size - (unsigned)id,
        TRUE, 0x00030000, sz, sz, LR_DEFAULTCOLOR);
    if (!h)
        h = CreateIconFromResourceEx((PBYTE)data + id, size - (unsigned)id,
            TRUE, 0x00030000, alt, alt, LR_DEFAULTCOLOR);
    return h;
}

/* Aguja baston (rectangular, punta plana, lume insertado). */
static void dibujar_baston(Graphics* g, unsigned ang, float len, float cola, float hw,
    float lume_desde, float lume_hasta)
{
    float s, c; sincos256(ang, &s, &c);
    /* eje de la aguja: direccion (c,s), perpendicular (nx,ny) = (-s,c) */
    const float nx = -s, ny = c;
    /* punta (centro del corte perpendicular en len) y base (centro en cola) */
    const float px = DCX + c * len, py = DCY + s * len;
    const float qx = DCX - c * cola, qy = DCY - s * cola;

    const float w = hw;
    /* punto cercano al centro donde arrancan las facetas (5% de len):
       deja un pequeno tramo cuadrado pegado al pivote, como en el real. */
    const float l0 = len * 0.05f;

    /* (1) Cuerpo exterior en acero (contorno plateado).  Rectangulo puro:
       mismo semiancho w en la punta y en la base. */
    const PointF outline[4] = {
        PointF(px + nx * w, py + ny * w),
        PointF(qx + nx * w, qy + ny * w),
        PointF(qx - nx * w, qy - ny * w),
        PointF(px - nx * w, py - ny * w)
    };
    g->FillPolygon(g_brOsc, outline, 4);

    /* (2) Acero de los flancos: GRIS medio, claramente mas oscuro que el
       lume, para que el canal blanco contraste como en la foto del real
       (antes la faceta cubria el 82% de la anchura en tono casi blanco y
       la aguja entera se veia lavada, sin lume distinguishable). */
    const float w2 = w * 0.80f;
    const PointF steel[4] = {
        PointF(px + nx * w2, py + ny * w2),
        PointF(qx + nx * w2, qy + ny * w2),
        PointF(qx - nx * w2, qy - ny * w2),
        PointF(px - nx * w2, py - ny * w2)
    };
    g->FillPolygon(g_brMid, steel, 4);

    /* (3) Faceta iluminada central (reflejo del pulido).  Solo se ve en el
       tramo metalico interior: en el tramo exterior el lume, mas ancho,
       la cubre por completo. */
    const float w3 = w * 0.42f;
    const PointF lit[4] = {
        PointF(px + nx * w3, py + ny * w3),
        PointF(DCX + c * l0 + nx * w3, DCY + s * l0 + ny * w3),
        PointF(DCX + c * l0 - nx * w3, DCY + s * l0 - ny * w3),
        PointF(px - nx * w3, py - ny * w3)
    };
    g->FillPolygon(g_brCla, lit, 4);

    /* (4) Canal de LUME blanco: ANCHO, ~la mitad de la anchura del baston
       (0.50 * hw), como en el Datejust real.  Ocupa la zona media-externa
       del baston, de lume_desde a lume_hasta (medidos desde el centro),
       dejando metal visto en la punta y otro tramo metalico hacia el
       centro: tres zonas, metal - lume - metal, como en la foto real. */
    const float wl = w * 0.50f;
    const float l1 = len * lume_desde;
    const float l2 = len * lume_hasta;
    const PointF lume[4] = {
        PointF(DCX + c * l1 - nx * wl, DCY + s * l1 - ny * wl),
        PointF(DCX + c * l2 - nx * wl, DCY + s * l2 - ny * wl),
        PointF(DCX + c * l2 + nx * wl, DCY + s * l2 + ny * wl),
        PointF(DCX + c * l1 + nx * wl, DCY + s * l1 + ny * wl)
    };
    g->FillPolygon(g_brLume, lume, 4);
}

/* Segundero: aguja fina tipo alfiler que TERMINA EN PUNTA (LineCapTriangle
   en el extremo), SIN bola ni contrapeso en ningun extremo, como en la
   foto del Datejust real: solo la linea afilada con su brillo central y
   la cola corta al otro lado del pivote.  Los Pen estan cacheados. */
static void dibujar_segundero(Graphics* g, unsigned ang)
{
    float s, c; sincos256(ang, &s, &c);
    const float px = DCX + c * R_S, py = DCY + s * R_S;
    const float qx = DCX - c * COL_S, qy = DCY - s * COL_S;

    /* Cuerpo de la segundera: LineCapTriangle en la punta, LineCapRound
       en la base (pegada al pivote). */
    g->DrawLine(g_penSBody, qx, qy, px, py);
    /* Brillo fino central del segundero (acero claro), con la misma forma. */
    g->DrawLine(g_penSLit, qx, qy, px, py);
}

static void dibujar_centro(Graphics* g)
{
    g->FillEllipse(g_brOsc, DCX - CEN_RO, DCY - CEN_RO, CEN_RO * 2, CEN_RO * 2);
    g->FillEllipse(g_brMid, DCX - CEN_RI * 1.2f, DCY - CEN_RI * 1.2f, CEN_RI * 2.4f, CEN_RI * 2.4f);
    g->FillEllipse(g_brCla, DCX - CEN_RI, DCY - CEN_RI, CEN_RI * 2, CEN_RI * 2);
    g->FillEllipse(g_brHi, DCX - CEN_PN, DCY - CEN_PN, CEN_PN * 2, CEN_PN * 2);
}

/* Digitos vectoriales limpios y ligeramente mas pequenos. */
static void dibujar_digito(Graphics* g, int dig, float x, float y, float sc)
{
    if (dig < 0 || dig > 9) return;
    const float w = 5.0f * sc, h = 7.0f * sc, t = sc * 0.95f;
    GraphicsPath path;
    path.SetFillMode(FillModeWinding);
    auto R = [&](float x0, float y0, float ww, float hh) {
        path.AddRectangle(RectF(x0, y0, ww, hh));
    };
    const float L = x + sc * 0.40f;
    const float RR = x + w - sc * 0.40f - t;
    const float T = y + sc * 0.30f;
    const float B = y + h - sc * 0.30f - t;
    const float M = y + h * 0.5f - t * 0.5f;
    const float CX = x + w * 0.5f - t * 0.5f;

    switch (dig) {
    case 0: {
        GraphicsPath o, i;
        o.AddEllipse(RectF(L, T, RR - L + t, B - T + t));
        float in = t * 0.92f;
        i.AddEllipse(RectF(L + in, T + in, RR - L + t - 2 * in, B - T + t - 2 * in));
        path.AddPath(&o, false);
        path.AddPath(&i, false);
        path.SetFillMode(FillModeAlternate);
        break;
    }
    case 1: R(CX, T, t, B - T + t); R(CX - t * 0.65f, T, t * 1.6f, t * 0.65f); break;
    case 2: R(L, T, RR - L + t, t); R(RR, T, t, M - T + t); R(L, M, RR - L + t, t); R(L, M, t, B - M + t); R(L, B, RR - L + t, t); break;
    case 3: R(L, T, RR - L + t, t); R(RR, T, t, B - T + t); R(L + t * 0.5f, M, RR - L - t * 0.5f + t, t); R(L, B, RR - L + t, t); break;
    case 4: R(L, T, t, M - T + t); R(L, M, RR - L + t, t); R(RR, T, t, B - T + t); break;
    case 5: R(L, T, RR - L + t, t); R(L, T, t, M - T + t); R(L, M, RR - L + t, t); R(RR, M, t, B - M + t); R(L, B, RR - L + t, t); break;
    case 6: R(L, T, RR - L + t, t); R(L, T, t, B - T + t); R(L, M, RR - L + t, t); R(RR, M, t, B - M + t); R(L, B, RR - L + t, t); break;
    case 7: R(L, T, RR - L + t, t); R(RR, T, t, B - T + t); break;
    case 8: R(L, T, t, B - T + t); R(RR, T, t, B - T + t); R(L, T, RR - L + t, t); R(L, M, RR - L + t, t); R(L, B, RR - L + t, t); break;
    case 9: R(L, T, RR - L + t, t); R(L, T, t, M - T + t); R(RR, T, t, B - T + t); R(L, M, RR - L + t, t); R(L, B, RR - L + t, t); break;
    }
    g->FillPath(g_brNeg, &path);
}

/* Aumento del cyclops 2.5x. */

static void dibujar_fecha_base(Graphics* g, int day)
{
    const int d1 = day / 10, d2 = day % 10;
    /* dig_sc = 3.1 -> mas pequenos y centrados como en la foto real */
    const float dig_sc = 3.1f;
    const float dig_w = 5.0f * dig_sc;
    const float gap = dig_sc * 1.4f;
    const float total_w = 2.0f * dig_w + gap;
    const float fx = VEN_CX - total_w * 0.5f;
    const float fy = VEN_CY - 3.5f * dig_sc;
    g->FillRectangle(g_brLupa, VEN_CX - VEN_W * 0.5f, VEN_CY - VEN_H * 0.5f, VEN_W, VEN_H);
    dibujar_digito(g, d1, fx, fy, dig_sc);
    dibujar_digito(g, d2, fx + dig_w + gap, fy, dig_sc);
}

/* Pinta el contenido amplificado 2.5x del cyclops: toma del fondo LIMPIO
   (g_limpia, con la fecha a escala 1 ya cocinada pero SIN agujas) el
   rectangulo central que ve la lente (VEN_W x VEN_H = 54x28) y lo
   vuelca escalado en el area de la lupa (LUP_W x LUP_H = 135x70).  Como
   la fuente ya lleva la fecha a tamano real, al amplificarla la fecha sale
   2.5x bajo la lupa.
   Las agujas NO estan en g_limpia: se redibujan aparte amplificadas. */
static void dibujar_lupa_contenido(Graphics* g)
{
    if (!g_limpia) return;

    /* Tamano del trozo de fondo que ve la lente: la lupa es LUP_W x LUP_H y
       amplia LUP_ZOOM (2.5x), asi que muestrea una region LUP_W/2.5 x
       LUP_H/2.5 = VEN_W x VEN_H (54x28), centrada en la ventanita. */
    const REAL src_w = (REAL)(LUP_W / LUP_ZOOM);   /* = VEN_W = 54 */
    const REAL src_h = (REAL)(LUP_H / LUP_ZOOM);   /* = VEN_H = 28 */
    const REAL sx = (REAL)(VEN_CX)-src_w * 0.5f;
    const REAL sy = (REAL)(VEN_CY)-src_h * 0.5f;

    const RectF destRect((REAL)(LUP_X0), (REAL)(LUP_Y0),
        (REAL)(LUP_W), (REAL)(LUP_H));

    /* Fondo blanco de la lente para que no asome nada fuera del muestreo. */
    g->FillRectangle(g_brLupa, destRect);

    g->SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g->DrawImage(g_limpia, destRect, sx, sy, src_w, src_h, UnitPixel);
}

/* Aplica la transformacion del cyclops (escala LUP_ZOOM con centro en el
   centro de la lente, recortada a su area) y llama a body().  Template con
   callable: admite lambdas con captura, asi que no hacen falta los
   trampolines ni el struct global de argumentos de antes. */
template <typename F>
static void con_transform_cyclops(Graphics* g, F&& body)
{
    const float cx = LUP_X0 + LUP_W * 0.5f;
    const float cy = LUP_Y0 + LUP_H * 0.5f;
    const GraphicsState st = g->Save();
    g->SetClip(RectF(LUP_X0, LUP_Y0, LUP_W, LUP_H));
    g->TranslateTransform(cx, cy);
    g->ScaleTransform(LUP_ZOOM, LUP_ZOOM);
    g->TranslateTransform(-cx, -cy);
    body(g);
    g->Restore(st);
}

/* Comprueba si una aguja (minutero o segundero) pasa por la zona de la
   lupa.  R_M y R_S son mayores que la distancia del centro a la ventanita
   (VEN_CX - DCX), asi que ambas la cruzan cuando su angulo se acerca
   a las 3h.  El horario es demasiado corto y no llega: se omite. */
static bool aguja_pasa_lupa(unsigned ang, float len)
{
    /* ang en 1/256 de vuelta, 64 = las 3h.  Margen proporcional a la
       anchura de la lupa vista desde el centro. */
    const float RAD_TO_256 = 256.0f / (2.0f * 3.14159265f);  /* rad -> 1/256 */
    const float dist = VEN_CX - DCX;                         /* centro->ventana */
    const float semi = (LUP_W * 0.5f) / dist;                /* semiarco de la lupa */
    int margen = (int)(semi * RAD_TO_256);
    if (margen < 12) margen = 12;                            /* ~17 grados: minimo */
    int d = ((int)ang - 64 + 256) & 255;                     /* 0..255 alrededor de 3h */
    if (d > 128) d -= 256;                                   /* -> -128..127 */
    return (abs(d) < margen) && (len > dist * 0.9f);
}

static void dibujar_cyclops(Graphics* g, unsigned am, unsigned as)
{
    /* (1) El interior de la lupa: amplifica 2.5x el trozo de fondo LIMPIO
       (con la fecha ya cocinada, sin agujas) que ve. */
    dibujar_lupa_contenido(g);

    /* (2) Si alguna aguja pasa por la lupa, se redibuja AMPLIFICADA 2.5x y
       recortada al interior de la lente, encima de la fecha: la aguja
       completa (baston facetado con lume, segundera afilada) con la misma
       geometria de fuera pero escalada. */
    if (aguja_pasa_lupa(am, R_M))
        con_transform_cyclops(g, [&](Graphics* gg) {
        dibujar_baston(gg, am, R_M, COL_M, HW_M, LUME_DESDE_M, LUME_HASTA_M);
            });
    if (aguja_pasa_lupa(as, R_S))
        con_transform_cyclops(g, [&](Graphics* gg) { dibujar_segundero(gg, as); });

    /* (3) Aro del cristal: claro arriba e izquierda, oscuro abajo y
       derecha, con un filete de brillo en lo alto.   */
    g->DrawRectangle(g_penAro1, LUP_X0 - 1.0f, LUP_Y0 - 1.0f, LUP_W + 2, LUP_H + 2);
    g->DrawRectangle(g_penAro2, LUP_X0 + 0.5f, LUP_Y0 + 0.5f, LUP_W - 1, LUP_H - 1);
    g->DrawLine(g_penHi, LUP_X0 + 6, LUP_Y0 + 3, LUP_X0 + LUP_W - 6, LUP_Y0 + 3);
}

static void componer(Graphics* g, unsigned ah, unsigned am, unsigned as, int day)
{
    g->SetSmoothingMode(SmoothingModeAntiAlias);
    g->SetPixelOffsetMode(PixelOffsetModeHalf);

    /* g_limpia es el fondo con la fecha ya cocinada (igual que la ESFERA
       limpia).  Se reconstruye SOLO cuando cambia el dia; el
       resto de los fotogramas se reutiliza.  La lupa muestrea de aqui, no
       de g_back, para que las agujas no se vean dos veces (sin amplificar +
       amplificadas) bajo el cristal. */
    if (g_limpia && day != g_day_limpia) {
        Graphics gl(g_limpia);
        gl.SetSmoothingMode(SmoothingModeAntiAlias);
        gl.SetPixelOffsetMode(PixelOffsetModeHalf);
        gl.DrawImage(g_bg, 0, 0, BG_W, BG_H);
        dibujar_fecha_base(&gl, day);
        g_day_limpia = day;
    }

    g->DrawImage(g_limpia ? g_limpia : g_bg, 0, 0, BG_W, BG_H);

    /* La fecha NO se vuelve a pintar en g_back: ya esta en g_limpia, que
       es lo que se acaba de volcar.  Las agujas si van encima. */
    dibujar_baston(g, ah, R_H, COL_H, HW_H, LUME_DESDE_H, LUME_HASTA_H);  /* horaria */
    dibujar_baston(g, am, R_M, COL_M, HW_M, LUME_DESDE_M, LUME_HASTA_M);  /* minutera */
    dibujar_segundero(g, as);
    dibujar_centro(g);
    dibujar_cyclops(g, am, as);
}

static void entrar_fullscreen(HWND hwnd)
{
    if (g_fullscreen) return;
    g_oldStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
    GetWindowRect(hwnd, &g_winRect);
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_FRAMECHANGED);
    g_fullscreen = true;
}

static void salir_fullscreen(HWND hwnd)
{
    if (!g_fullscreen) return;
    SetWindowLongPtrW(hwnd, GWL_STYLE, g_oldStyle);
    SetWindowPos(hwnd, NULL, g_winRect.left, g_winRect.top,
        g_winRect.right - g_winRect.left, g_winRect.bottom - g_winRect.top, SWP_FRAMECHANGED);
    g_fullscreen = false;
}

/* ==================== tic-tac (sonido) ==================== */
/* El tic-tac de un reloj mecanico real no es un tono sinusoidal sostenido
   (eso es lo que produce Beep() y suena a pitido electronico), sino dos
   CLIC secos y cortos: el choque de la rueda de escape contra el pallet.
   Cada clic dura ~30-50 ms y tiene:
     - Un transitorio inicial muy breve (~3 ms) con energia de alta
       frecuencia (el "click" metalico) que decae exponencialmente.
     - Una resonancia principal de baja frecuencia (~150-300 Hz) que
       amortigua en ~30 ms.
     - Diferencia tic/tac: el tic es algo mas agudo y brillante; el tac
       mas grave y sordo.
   Para sintetizarlo en Win32 sin archivos externos se genera un WAV PCM
   16-bit mono 44.1 kHz en memoria (cabecera RIFF + datos) y se reproduce
   con PlaySound(SND_MEMORY), que es la API mas ligera y portable.  Se
   pregeneran los dos buffers (tic y tac) al arrancar y se reutilizan en
   cada pulso: el hilo solo alterna entre uno y otro cada 125 ms, como un
   automatico a 28.800 alternancias/hora (8 ticks/s en el Datejust real).
   El hilo sale solo al poner g_tictac_run=false. */

   /* Bandera de parada del hilo: atomica porque el worker la lee y el hilo
      principal la escribe; std::atomic<bool> lo hace correcto y portable. */
static std::atomic<bool> g_tictac_run{ false };
static HANDLE g_tictac_h = NULL;

/* Buffers WAV pregenerados (cabecera + PCM). */
static unsigned char* g_wav_tic = NULL;   /* clic agudo */
static unsigned char* g_wav_tac = NULL;   /* clic grave */
static size_t g_wav_tic_len = 0;
static size_t g_wav_tac_len = 0;

/* Genera WAV PCM 16-bit mono 44.1 kHz: clic seco de escape 28800 bph. */
static unsigned char* generar_clic_wav(unsigned freq, float brillo,
    float dur_ms, size_t* out_len)
{
    const unsigned SR = 44100;       /* sample rate */
    const unsigned N = (unsigned)(SR * dur_ms / 1000.0f);
    /* WAV 16-bit mono: 44 bytes de cabecera + 2*N bytes de datos */
    const size_t data_len = (size_t)N * 2;
    const size_t wav_len = 44 + data_len;
    unsigned char* buf = (unsigned char*)malloc(wav_len);
    if (!buf) { *out_len = 0; return NULL; }

    /* --- Cabecera RIFF / WAVE / fmt / data --- */
    memcpy(buf + 0, "RIFF", 4);
    *(unsigned*)(buf + 4) = (unsigned)(wav_len - 8);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    *(unsigned*)(buf + 16) = 16;          /* tamano subchunk fmt */
    *(unsigned short*)(buf + 20) = 1;    /* PCM */
    *(unsigned short*)(buf + 22) = 1;    /* mono */
    *(unsigned*)(buf + 24) = SR;
    *(unsigned*)(buf + 28) = SR * 2;     /* byte rate: 16-bit mono */
    *(unsigned short*)(buf + 32) = 2;    /* block align */
    *(unsigned short*)(buf + 34) = 16;   /* bits por sample */
    memcpy(buf + 36, "data", 4);
    *(unsigned*)(buf + 40) = (unsigned)data_len;

    /* Sintesis de clic seco (escape 28800 bph). */
    /* Clic seco realista (escape 28800 bph) */
    const float tau_dr = 0.0068f * SR;
    const float two_pi_fc = 6.28318530718f * freq / (float)SR;
    const float two_pi_fm = two_pi_fc * 14.5f;
    const float I0 = 4.8f * (0.50f + 0.50f * brillo);
    short* pcm = (short*)(buf + 44);
    for (unsigned n = 0; n < N; n++) {
        const float t = (float)n / (float)SR;
        const float env = expf(-(float)n / tau_dr);
        const float I = I0 * expf(-t * 220.0f);
        float y = env * sinf(two_pi_fc * (float)n + I * sinf(two_pi_fm * (float)n));
        if (n < 18) {
            float click = (1.0f - (float)n / 18.0f);
            y += 0.42f * click * env * ((n & 1) ? 1.0f : -1.0f);
        }
        if (n < 2) y *= (float)n / 2.0f;
        if (y > 0.75f) y = 0.75f;
        if (y < -0.75f) y = -0.75f;
        pcm[n] = (short)(y * 23000.0f);
    }
    *out_len = wav_len;
    return buf;
}

static DWORD WINAPI hilo_tic_tac(LPVOID)
{
    /* Cadencia: 8 ticks por segundo (cada 125 ms), tic/tac alternados.
       Es la cadencia REAL del Datejust: 8 alternancias/s = 28.800 alt/h.
       Espera en tramos cortos para salir rapido al cerrar. */
    unsigned pulso = 0;
    while (g_tictac_run) {
        /* tic (pares): portadora 315 Hz + moduladora 15x = 4.7 kHz
           tac (impares): portadora 190 Hz + moduladora 15x = 2.85 kHz */
        const unsigned char* wav = (pulso & 1u) ? g_wav_tac : g_wav_tic;
        if (wav) PlaySoundA((LPCSTR)wav, NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
        pulso++;
        /* 125 ms en total: 5 tramos de 25 ms para poder salir rapido */
        for (int i = 0; i < 5 && g_tictac_run; i++) Sleep(25);
    }
    /* silencia al salir */
    PlaySoundA(NULL, NULL, 0);
    return 0;
}
static void iniciar_tic_tac(void)
{
    if (g_tictac_h) return;
    /* Tic mas agudo, tac mas grave (28800 bph). */
    if (!g_wav_tic) g_wav_tic = generar_clic_wav(355, 0.97f, 11.5f, &g_wav_tic_len);
    if (!g_wav_tac) g_wav_tac = generar_clic_wav(215, 0.75f, 13.0f, &g_wav_tac_len);
    g_tictac_run = true;
    g_tictac_h = CreateThread(NULL, 0, hilo_tic_tac, NULL, 0, NULL);
}
static void detener_tic_tac(void)
{
    if (!g_tictac_h) return;
    g_tictac_run = false;
    WaitForSingleObject(g_tictac_h, 500);
    CloseHandle(g_tictac_h); g_tictac_h = NULL;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, 1, 80, NULL);  /* ~12 fps suficiente */
        iniciar_tic_tac();   /* hilo del sonido tic-tac (en paralelo) */
        return 0;
    case WM_TIMER: {
        unsigned ah, am, as; int day;
        angulos_ahora(&ah, &am, &as, &day);
        if (ah != g_ah || am != g_am || as != g_as || day != g_day) {
            g_ah = ah; g_am = am; g_as = as; g_day = day;
            g_needRedraw = true;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
        /* Si g_back no esta listo (no deberia pasar tras WM_CREATE, pero
           defensivo): salir limpio sin dibujar nada stale. */
        if (!g_back) { EndPaint(hwnd, &ps); return 0; }
        if (g_needRedraw && g_bg && g_limpia) {
            Graphics gg(g_back);
            componer(&gg, g_ah, g_am, g_as, g_day);
            g_needRedraw = false;
        }
        {
            Graphics gs(hdc);
            RECT rc; GetClientRect(hwnd, &rc);
            const int cw = rc.right, ch = rc.bottom;
            const float sc = (std::min)((float)cw / BG_W, (float)ch / BG_H);
            const int dw = (int)(BG_W * sc + 0.5f), dh = (int)(BG_H * sc + 0.5f);
            const int ox = (cw - dw) / 2, oy = (ch - dh) / 2;
            if (ox > 0 || oy > 0 || ox + dw < cw || oy + dh < ch) {
                if (oy > 0)      gs.FillRectangle(g_brBlack, 0, 0, cw, oy);
                if (oy + dh < ch)  gs.FillRectangle(g_brBlack, 0, oy + dh, cw, ch - (oy + dh));
                if (ox > 0)      gs.FillRectangle(g_brBlack, 0, oy, ox, dh);
                if (ox + dw < cw)  gs.FillRectangle(g_brBlack, ox + dw, oy, cw - (ox + dw), dh);
            }
            gs.SetInterpolationMode(InterpolationModeBilinear);
            gs.DrawImage(g_back, ox, oy, dw, dh);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MAXIMIZE) { entrar_fullscreen(hwnd); return 0; }
        if ((wp & 0xFFF0) == SC_RESTORE) { salir_fullscreen(hwnd); return 0; }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (g_fullscreen) salir_fullscreen(hwnd);
            else PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        else if (wp == VK_F11) {
            if (g_fullscreen) salir_fullscreen(hwnd);
            else entrar_fullscreen(hwnd);
        }
        return 0;
    case WM_DESTROY:
        detener_tic_tac();   /* apaga el hilo del sonido */
        KillTimer(hwnd, 1); PostQuitMessage(0); return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow)
{
    GdiplusStartupInput si;
    if (GdiplusStartup(&g_token, &si, NULL) != Ok) {
        MessageBoxW(NULL, L"GDI+ no disponible.", L"ROLEXWIN", MB_ICONERROR);
        return 1;
    }

    g_bg = LoadBitmapFromMemory(bg_data, bg_data_len);
    g_hIcon = LoadIconFromMemory(ico_data, ico_data_len, 32);
    g_hIconSm = LoadIconFromMemory(ico_data, ico_data_len, 16);
    if (!g_bg) {
        MessageBoxW(NULL, L"Error cargando fondo embebido.", L"ROLEXWIN", MB_ICONERROR);
        if (g_hIconSm) DestroyIcon(g_hIconSm);
        if (g_hIcon)   DestroyIcon(g_hIcon);
        GdiplusShutdown(g_token); return 1;
    }
    g_back = new Bitmap(BG_W, BG_H, PixelFormat32bppARGB);
    g_limpia = new Bitmap(BG_W, BG_H, PixelFormat32bppARGB);
    if (!g_back || !g_limpia) {
        MessageBoxW(NULL, L"Error creando bitmaps.", L"ROLEXWIN", MB_ICONERROR);
        delete g_limpia; delete g_back; delete g_bg;
        if (g_hIconSm) DestroyIcon(g_hIconSm);
        if (g_hIcon)   DestroyIcon(g_hIcon);
        GdiplusShutdown(g_token); return 1;
    }
    /* g_limpia: fondo con la fecha ya cocinada (sin agujas).  Se
       reconstruye solo al cambiar el dia; la lupa muestrea de aqui. */

    g_brOsc = new SolidBrush(C_OSC);
    g_brMid = new SolidBrush(C_MID);
    g_brCla = new SolidBrush(C_CLA);
    g_brHi = new SolidBrush(C_HI);
    g_brNeg = new SolidBrush(C_NEG);
    g_brLume = new SolidBrush(C_LUME);
    g_brLupa = new SolidBrush(C_LUPA);
    g_brBlack = new SolidBrush(Color(0, 0, 0));
    g_penAro1 = new Pen(C_ARO1, 2.5f * SCALE);
    g_penAro2 = new Pen(C_ARO2, 1.6f * SCALE);
    g_penHi = new Pen(Color(170, 255, 255, 255), 1.5f * SCALE);
    /* Pens del segundero: cuerpo (HW_S) y brillo central (0.35*HW_S),
       LineCapRound en la base y LineCapTriangle en la punta. */
    g_penSBody = new Pen(C_OSC, HW_S);
    g_penSBody->SetStartCap(LineCapRound);
    g_penSBody->SetEndCap(LineCapTriangle);
    g_penSLit = new Pen(C_HI, HW_S * 0.35f);
    g_penSLit->SetStartCap(LineCapRound);
    g_penSLit->SetEndCap(LineCapTriangle);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"RolexWinClass";
    /* Iconos de la clase: el grande para la barra de titulo/Alt-Tab, el
       pequeno (hIconSm) para la barra de tareas.  Si el .ICO no cargara,
       se cae al IDI_APPLICATION por defecto. */
    wc.hIcon = g_hIcon ? g_hIcon : LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm = g_hIconSm ? g_hIconSm : (g_hIcon ? g_hIcon : LoadIcon(NULL, IDI_APPLICATION));
    RegisterClassExW(&wc);

    const int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    const float sc = (std::min)(0.70f * sw / BG_W, 0.88f * sh / BG_H);
    const int ww = (int)(BG_W * sc) + GetSystemMetrics(SM_CXFRAME) * 2;
    const int wh = (int)(BG_H * sc) + GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFRAME) * 2;

    g_hwnd = CreateWindowExW(WS_EX_COMPOSITED, L"RolexWinClass",
        L"ROLEXWIN - Rolex Oyster Perpetual Datejust",
        WS_OVERLAPPEDWINDOW, (sw - ww) / 2, (sh - wh) / 2, ww, wh, NULL, NULL, hInst, NULL);
    if (!g_hwnd) { GdiplusShutdown(g_token); return 1; }

    if (g_hIcon)   SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_hIcon);
    if (g_hIconSm) SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_hIconSm);

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    angulos_ahora(&g_ah, &g_am, &g_as, &g_day);
    g_needRedraw = true;
    InvalidateRect(g_hwnd, NULL, FALSE);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    delete g_penAro2; delete g_penAro1; delete g_penHi;
    delete g_penSLit;  delete g_penSBody;
    delete g_brBlack; delete g_brLupa; delete g_brLume; delete g_brNeg;
    delete g_brHi; delete g_brCla; delete g_brMid; delete g_brOsc;
    delete g_limpia; delete g_back; delete g_bg;
    /* PlaySoundA(SND_ASYNC) es fire-and-forget: el SO puede seguir leyendo
       los buffers despues de que volvamos de PlaySoundA.  Llamamos a
       PlaySoundA(NULL) para detener cualquier reproduccion en curso ANTES
       de liberar la memoria, evitando un use-after-free.  detener_tic_tac
       ya hizo lo mismo al cerrar la ventana, pero repetirlo aqui cubre el
       caso en que el hilo emitio un clic entre WM_DESTROY y este punto. */
    PlaySoundA(NULL, NULL, 0);
    free(g_wav_tic); g_wav_tic = NULL;
    free(g_wav_tac); g_wav_tac = NULL;
    if (g_hIconSm) DestroyIcon(g_hIconSm);
    if (g_hIcon) DestroyIcon(g_hIcon);
    GdiplusShutdown(g_token);
    return (int)msg.wParam;
}