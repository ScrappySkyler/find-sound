// Mod de PARRY con BARRA DE POSTURA (estilo Sekiro) para Dusklight
//
// - Empujas el escudo (procGuardAttackInit) -> ventana corta de parry.
// - Cada parry NO aturde al enemigo: sigue atacando. Sube su barra (se llena poco a poco).
// - Con PARRIES_TO_STUN parries la barra se llena y el enemigo queda aturdido.
// - Aturdido: B = tajo relampago (Mortal Draw), B otra vez = segundo tajo.
// - Tras los tajos (o si se acaba el tiempo) la barra se reinicia.
// - La barra se muestra sobre el enemigo fijado con Z.
// - Sin escudo automatico al fijar enemigos.
// - Tras un parry que no llena la barra, Link no puede atacar con la espada un rato:
//   solo se puede golpear al enemigo cuando queda aturdido (barra llena).

#define ENABLE_BAR 1   // 1 = dibuja la barra sobre el enemigo, 0 = sin barra (solo sonidos)
#define AUDIO_SCAN 0   // 1 = herramienta para encontrar el ID del sonido (temporal), 0 = apagada
#define AUDIO_REPLACE 0   // 1 = reemplaza el sonido del parry por res/parry_success.wav (descartado)
#define SOUND_LOG 1       // 1 = anota en el registro el ID de cada sonido que suena (ACTIVADO)

#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cmath>

#include "mods/service.hpp"
#include "mods/svc/log.h"
#include "mods/svc/hook.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_audio.h"
#include "SSystem/SComponent/c_math.h"

#if ENABLE_BAR
#include "d/d_drawlist.h"
#include "m_Do/m_Do_lib.h"
#include <dolphin/gx.h>
#endif

#if AUDIO_REPLACE
#include "mods/svc/audio_res.h"
#endif

#if SOUND_LOG
#include <map>
#include <cstdio>
#include "Z2AudioLib/Z2SeMgr.h"
#endif

#if AUDIO_SCAN
#include <map>
#include <string>
#include <cstdio>
#endif

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
#if AUDIO_REPLACE
IMPORT_OPTIONAL_SERVICE(AudioResService, svc_audio_res);   // si falta, el mod carga igual, sin el sonido
#endif

DEFINE_HOOK(&daAlink_c::execute, LinkExecute);
DEFINE_HOOK(&daAlink_c::procGuardAttackInit, GuardAttackInit);
DEFINE_HOOK(&daAlink_c::setGuardSe, GuardSe);
DEFINE_HOOK(&daAlink_c::procGuardSlipInit, GuardSlipInit);
DEFINE_HOOK(&daAlink_c::procGuardBreakInit, GuardBreakInit);
DEFINE_HOOK(&daAlink_c::setSmallGuard, SmallGuard);
DEFINE_HOOK(&daAlink_c::setShieldGuard, ShieldGuard);
// Ataques con espada que se bloquean tras un parry
DEFINE_HOOK(&daAlink_c::procCutNormalInit, CutNormalInit);
DEFINE_HOOK(&daAlink_c::procCutFinishInit, CutFinishInit);
DEFINE_HOOK(&daAlink_c::procCutJumpInit, CutJumpInit);
DEFINE_HOOK(&daAlink_c::procCutTurnInit, CutTurnInit);
DEFINE_HOOK(&daAlink_c::procCutTurnChargeInit, CutTurnChargeInit);
DEFINE_HOOK(&daAlink_c::procCutDownInit, CutDownInit);
DEFINE_HOOK(&daAlink_c::procCutHeadInit, CutHeadInit);
DEFINE_HOOK(&daAlink_c::procCutLargeJumpChargeInit, CutLargeJumpChargeInit);
DEFINE_HOOK(&daAlink_c::procCutLargeJumpInit, CutLargeJumpInit);
DEFINE_HOOK(&daAlink_c::setCutDash, CutDash);
#if ENABLE_BAR
DEFINE_HOOK(&daAlink_c::draw, LinkDraw);
#endif
#if SOUND_LOG
DEFINE_HOOK(&Z2SeMgr::seStart, SeStartLog);
#endif
#if AUDIO_SCAN
DEFINE_HOOK_SYMBOL("JASBasicWaveBank::getWaveHandle", void*(void*, uint32_t), ScanBasicWave);
DEFINE_HOOK_SYMBOL("JASSimpleWaveBank::getWaveHandle", void*(void*, uint32_t), ScanSimpleWave);
#endif

// ---- Ajustes (ticks de logica: 30 por segundo) ----
static const int PARRY_WINDOW_TICKS = 5;     // ventana tras empujar el escudo
static const int PARRIES_TO_STUN = 3;        // parries para llenar la barra
static const int STUN_TICKS = 120;           // tiempo aturdido para empezar los tajos (4 s)
static const int SECOND_SLASH_TICKS = 30;    // tiempo para el segundo tajo
static const int HOLD_AFTER_TICKS = 20;      // enemigo quieto mientras cae el segundo tajo
static const int ATTACK_LOCK_TICKS = 60;     // sin atacar tras un parry normal (60 = 2 s)

// ---- Ajustes de la barra ----
static const float BAR_HEIGHT_ABOVE_HEAD = 60.0f;  // altura sobre la cabeza (unidades del juego)
static const float BAR_WIDTH = 120.0f;             // ancho en pixeles
static const float POS_SMOOTHING = 0.40f;          // 0-1: menor = mas suave (y mas retraso)
static const float FILL_SMOOTHING = 0.18f;         // velocidad a la que se llena la barra

#if AUDIO_REPLACE
// Muestra de audio que se reemplaza (la encontrada con la herramienta de escaneo).
static const AudioWaveBank PARRY_WAVE_BANK = AUDIO_WAVE_BANK_MUSIC_SAMPLES;
static const uint16_t PARRY_WAVE_ID = 172;
#endif

// Sonido del tercer parry (el que llena la barra): numero del sonido del juego encontrado
// con el registro. Pon 0 para usar el sonido normal.
static const uint32_t STUN_SE_ID = 0;   // cuando encuentres el sonido bueno, ponlo aqui (0 = modo de prueba)
#define STUN_SE_TEST 1                  // 1 = en cada aturdimiento prueba un sonido distinto de la lista
static const uint32_t STUN_SE_TEST_LIST[] = {90, 89, 88, 94, 87, 86, 14};
static const int STUN_SE_TEST_COUNT = 7;

// Valores del enum de tajos finales (Mortal Draw A y B)
static const int MORTAL_DRAW_A = 3;
static const int MORTAL_DRAW_B = 4;

// ---- Estado ----
struct EnemyState {
    int parries = 0;        // parries acumulados (llenado de la barra)
    int stunTimer = 0;      // aturdido: esperando el primer tajo
    int secondTimer = 0;    // ventana del segundo tajo
    int holdTimer = 0;      // quieto mientras cae el segundo tajo
    bool lastWasA = false;

    // Solo para dibujar la barra
    bool hasScreen = false;
    float sx = 0.0f, sy = 0.0f;   // posicion suavizada en pantalla
    float heightOff = 0.0f;       // altura de la cabeza suavizada
    float dispRatio = 0.0f;       // relleno mostrado (animado)
};

static std::unordered_map<uint32_t, EnemyState> g_enemies;

static int g_parryTimer = 0;
static bool g_parryHitThisTick = false;
static int g_attackLock = 0;        // ticks restantes sin poder atacar
static bool g_bypassLock = false;   // el propio mod lanza los tajos relampago

static fopAc_ac_c* actor_by_id(uint32_t id) {
    fopAc_ac_c* a = nullptr;
    fopAcM_SearchByID((fpc_ProcID)id, &a);
    return a;
}

static void on_bash_post(ModContext*, void*, void*, void*) {
    g_parryTimer = PARRY_WINDOW_TICKS;
}

// Un golpe pego en el escudo.
static HookAction on_guard_se_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);

    if (g_parryTimer <= 0) return HOOK_CONTINUE;

    if (link->mProcID == daAlink_c::PROC_SIDESTEP ||
        link->mProcID == daAlink_c::PROC_FRONT_ROLL ||
        link->mProcID == daAlink_c::PROC_SIDE_ROLL ||
        link->mProcID == daAlink_c::PROC_BACK_JUMP) {
        return HOOK_CONTINUE;
    }

    // PARRY!
    g_parryTimer = 0;
    g_parryHitThisTick = true;

    bool stunnedNow = false;
    bool lockAttacks = true;
    fopAc_ac_c* target = link->mTargetedActor;
    if (target) {
        EnemyState& st = g_enemies[fopAcM_GetID(target)];
        bool exposed = st.stunTimer > 0 || st.secondTimer > 0 || st.holdTimer > 0;
        if (exposed) lockAttacks = false;
        if (!exposed) {
            st.parries++;
            if (st.parries >= PARRIES_TO_STUN) {
                st.parries = PARRIES_TO_STUN;
                st.stunTimer = STUN_TICKS;
                stunnedNow = true;
            }
        }
    }

    // Parry que no llena la barra: no se puede atacar un rato. Si se lleno, se libera.
    g_attackLock = (stunnedNow || !lockAttacks) ? 0 : ATTACK_LOCK_TICKS;

    if (stunnedNow) {
        uint32_t seId = STUN_SE_ID;
#if STUN_SE_TEST
        static int s_testIdx = 0;
        if (seId == 0) {
            seId = STUN_SE_TEST_LIST[s_testIdx % STUN_SE_TEST_COUNT];
            char msg[64];
            snprintf(msg, sizeof(msg), "STUN: probando el sonido %u", seId);
            svc_log->info(mod_ctx, msg);
            s_testIdx++;
        }
#endif
        if (seId != 0) mDoAud_seStart(seId, nullptr, 0, 0);
        else link->setPlayerSe(Z2SE_TITLE_ENTER);
    } else {
        link->setPlayerSe(Z2SE_MIDNA_JUMP);
    }
    dComIfGp_getVibration().StartShock(VIBMODE_S_POWER4, 1, cXyz(0.0f, 1.0f, 0.0f));

    svc_log->info(mod_ctx, stunnedNow ? "PARRY: enemigo aturdido" : "PARRY");
    return HOOK_CONTINUE;
}

// En un parry cancelamos el retroceso / la guardia rota.
static HookAction skip_if_parry(ModContext*, void*, void* retval, void*) {
    if (!g_parryHitThisTick) return HOOK_CONTINUE;
    if (retval != nullptr) *static_cast<int*>(retval) = 0;
    return HOOK_SKIP_ORIGINAL;
}

// Bloquea el inicio de ataques con espada mientras dure el castigo del parry.
static HookAction block_attack_int(ModContext*, void*, void* retval, void*) {
    if (g_attackLock <= 0 || g_bypassLock) return HOOK_CONTINUE;
    if (retval != nullptr) *static_cast<int*>(retval) = 0;
    return HOOK_SKIP_ORIGINAL;
}

static HookAction block_attack_void(ModContext*, void*, void*, void*) {
    if (g_attackLock <= 0 || g_bypassLock) return HOOK_CONTINUE;
    return HOOK_SKIP_ORIGINAL;
}

// Quita el escudo automatico al fijar enemigos (la estructura derivada permite
// usar los miembros internos de Link).
struct GuardHelper : daAlink_c {
    static void remove_auto_guard(daAlink_c* l) {
        GuardHelper* h = static_cast<GuardHelper*>(l);
        if (h->checkAttentionLock() &&
            h->mProcID != PROC_GUARD_SLIP &&
            !h->checkSmallUpperGuardAnime()) {
            h->offNoResetFlg2(FLG2_UNK_8000000);
        }
    }
};

static void on_shield_guard_post(ModContext*, void* args, void*, void*) {
    GuardHelper::remove_auto_guard(mods::arg<daAlink_c*>(args, 0));
}

// ======================= HERRAMIENTA: REGISTRO DE SONIDOS =======================
// *** SISTEMA DE LOGGING MEJORADO ***
// Anota en el registro del mod el nombre y numero de cada sonido que el juego reproduce.
// Uso: deja el juego quieto unos 20 segundos (para que los ruidos de ambiente se anoten y se
// silencien), pulsa Clear, haz lo que quieras identificar y mira que numeros NUEVOS aparecen.
#if SOUND_LOG
static int g_tick = 0;
static std::map<uint64_t, int> g_soundCount;   // veces que se ha anotado cada sonido

static uint32_t swap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

// Decodifica el numero de sonido en sus componentes
// El formato es: [Seccion(8bits) | Grupo(8bits) | ID(16bits)]
static void log_sound_detailed(uint32_t number) {
    uint8_t section = (number >> 24) & 0xFF;
    uint8_t group = (number >> 16) & 0xFF;
    uint16_t id = number & 0xFFFF;
    
    // Contador para evitar spam de sonidos ambientales repetitivos
    uint64_t key = number;
    int& n = g_soundCount[key];
    if (++n > 3) return;  // Solo log de las primeras 3 veces
    
    char buf[256];
    snprintf(buf, sizeof(buf), 
             "[AUDIO] SND ID: %u (0x%08X) | Secc: %u | Grup: %u | SubID: %u%s",
             number, number, section, group, id,
             n > 1 ? " [REPETIDO]" : "");
    svc_log->info(mod_ctx, buf);
}

// Hook que captura TODOS los efectos de sonido (SE)
static HookAction on_se_start_log(ModContext*, void* args, void*, void*) {
    uint32_t seId = swap32(mods::arg<uint32_t>(args, 1));
    log_sound_detailed(seId);
    return HOOK_CONTINUE;
}
#endif  // SOUND_LOG

// ======================= HERRAMIENTA: BUSCAR ID DE SONIDO =======================
// Temporal. Mantener R y presionar Y (varias veces). Cada pulsacion hace una prueba distinta:
//   1 = no reproduce nada (linea base, para descartar la musica y el ambiente)
//   2 = reproduce MIDNA_JUMP y anota que muestras de audio se piden
//   3 = reproduce TITLE_ENTER y anota lo mismo
// Hazlo parado en un lugar tranquilo, sin enemigos. El resultado sale en el registro del mod.
#if AUDIO_SCAN
static const int SCAN_TICKS = 20;
static const int SCAN_MAX = 512;
struct ScanEntry { uintptr_t bank; uint32_t id; };
static ScanEntry g_scanBuf[SCAN_MAX];        // (banco, id de muestra) sin repetir
static volatile int g_scanCount = 0;
static volatile bool g_scanActive = false;
static int g_scanTicks = 0;
static int g_scanStep = 0;
static const char* g_scanLabel = "";

// Puede llamarse desde el hilo de audio; como es solo una herramienta de diagnostico,
// no usa candados (un choque raro solo perderia un dato).
static HookAction on_scan_wave(ModContext*, void* args, void*, void*) {
    if (!g_scanActive) return HOOK_CONTINUE;
    uintptr_t bank = (uintptr_t)mods::arg<void*>(args, 0);
    uint32_t id = mods::arg<uint32_t>(args, 1);
    int n = g_scanCount;
    for (int i = 0; i < n; i++) {
        if (g_scanBuf[i].bank == bank && g_scanBuf[i].id == id) return HOOK_CONTINUE;
    }
    if (n < SCAN_MAX) {
        g_scanBuf[n].bank = bank;
        g_scanBuf[n].id = id;
        g_scanCount = n + 1;
    }
    return HOOK_CONTINUE;
}

static void scan_start(daAlink_c* link, const char* label, bool playMidna, bool playTitle) {
    g_scanCount = 0;
    g_scanLabel = label;
    g_scanTicks = SCAN_TICKS;
    g_scanActive = true;
    if (playMidna) link->setPlayerSe(Z2SE_MIDNA_JUMP);
    if (playTitle) link->setPlayerSe(Z2SE_TITLE_ENTER);
}

static void scan_tick(daAlink_c* link) {
    bool r = mDoCPd_c::getHoldR(PAD_1);
    bool l = mDoCPd_c::getHoldL(PAD_1);
    if (g_scanTicks == 0 && mDoCPd_c::getTrigY(PAD_1)) {
        if (r && l) scan_start(link, "NADA (base)", false, false);
        else if (r) scan_start(link, "TITLE_ENTER", false, true);
        else if (l) scan_start(link, "MIDNA_JUMP", true, false);
    }
    if (g_scanTicks > 0 && --g_scanTicks == 0) {
        g_scanActive = false;
        std::map<uintptr_t, std::vector<uint32_t>> banks;
        int count = g_scanCount;
        for (int i = 0; i < count; i++) banks[g_scanBuf[i].bank].push_back(g_scanBuf[i].id);
        char head[96];
        snprintf(head, sizeof(head), "SCAN [%s]: %d banco(s)", g_scanLabel, (int)banks.size());
        svc_log->info(mod_ctx, head);
        int n = 0;
        for (auto& kv : banks) {
            std::string line = "SCAN [" + std::string(g_scanLabel) + "] banco " + std::to_string(++n) +
                               " (" + std::to_string(kv.second.size()) + " ids):";
            int shown = 0;
            for (uint32_t id : kv.second) {
                if (shown++ >= 80) { line += " ..."; break; }
                line += " " + std::to_string(id);
            }
            svc_log->info(mod_ctx, line.c_str());
        }
    }
}
#endif  // AUDIO_SCAN

// Cada tick de Link.
static HookAction on_execute_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);

    if (g_parryTimer > 0) g_parryTimer--;
    if (g_attackLock > 0) g_attackLock--;
#if SOUND_LOG
    g_tick++;
#endif
#if AUDIO_SCAN
    scan_tick(link);
#endif

    // Actualizar enemigos: limpiar muertos, mantener quietos a los aturdidos.
    for (auto it = g_enemies.begin(); it != g_enemies.end();) {
        fopAc_ac_c* a = actor_by_id(it->first);
        if (!a) {
            it = g_enemies.erase(it);
            continue;
        }
        EnemyState& st = it->second;

        if (st.stunTimer > 0) {
            a->speedF = 0.0f;
            if (--st.stunTimer == 0) st.parries = 0;       // se acabo el tiempo: reinicia
        } else if (st.secondTimer > 0) {
            a->speedF = 0.0f;
            if (--st.secondTimer == 0) st.parries = 0;     // no hubo segundo tajo: reinicia
        } else if (st.holdTimer > 0) {
            a->speedF = 0.0f;
            st.holdTimer--;
        }
        ++it;
    }

    // Tajos relampago sobre el enemigo fijado, si esta aturdido.
    fopAc_ac_c* target = link->mTargetedActor;
    if (target && mDoCPd_c::getTrigB(PAD_1)) {
        auto it = g_enemies.find(fopAcM_GetID(target));
        if (it != g_enemies.end()) {
            EnemyState& st = it->second;
            if (st.stunTimer > 0) {
                st.lastWasA = cM_rndF(1.0f) < 0.5f;
                g_bypassLock = true;
                link->procCutFinishInit(st.lastWasA ? MORTAL_DRAW_A : MORTAL_DRAW_B);
                g_bypassLock = false;
                st.stunTimer = 0;
                st.secondTimer = SECOND_SLASH_TICKS;
            } else if (st.secondTimer > 0) {
                g_bypassLock = true;
                link->procCutFinishInit(st.lastWasA ? MORTAL_DRAW_B : MORTAL_DRAW_A);
                g_bypassLock = false;
                st.secondTimer = 0;
                st.holdTimer = HOLD_AFTER_TICKS;
                st.parries = 0;                            // barra reiniciada
            }
        }
    }
    return HOOK_CONTINUE;
}

static void on_execute_post(ModContext*, void*, void*, void*) {
    g_parryHitThisTick = false;
}

// ======================= BARRA SOBRE EL ENEMIGO =======================
#if ENABLE_BAR

// Texturas de la barra (GX_TF_RGBA8, 256x20), generadas de tus imagenes.
static const int BAR_TEX_W = 256;
static const int BAR_TEX_H = 20;
// Zona interior de relleno dentro de la textura (fraccion horizontal 0-1).
static const float FILL_U0 = 0.0545f;
static const float FILL_U1 = 0.9525f;

alignas(32) static const unsigned char TEX_BAR_FULL[20480] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0[...]
alignas(32) static const unsigned char TEX_BAR_EMPTY[20480] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,[...]


struct BarInfo {
    float x, y;      // centro de la barra en pantalla
    float ratio;     // 0..1 (relleno mostrado)
};
static std::vector<BarInfo> g_bars;

static void draw_quad(float x0, float y0, float x1, float y1,
                      float u0, float u1, u8 alpha) {
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(x0, y0, 0.0f); GXColor4u8(255, 255, 255, alpha); GXTexCoord2f32(u0, 0.0f);
    GXPosition3f32(x1, y0, 0.0f); GXColor4u8(255, 255, 255, alpha); GXTexCoord2f32(u1, 0.0f);
    GXPosition3f32(x1, y1, 0.0f); GXColor4u8(255, 255, 255, alpha); GXTexCoord2f32(u1, 1.0f);
    GXPosition3f32(x0, y1, 0.0f); GXColor4u8(255, 255, 255, alpha); GXTexCoord2f32(u0, 1.0f);
    GXEnd();
}

static void bind_texture(GXTexObj* obj, const unsigned char* data) {
    GXInitTexObj(obj, (void*)data, BAR_TEX_W, BAR_TEX_H, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GXInitTexObjLOD(obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GXLoadTexObj(obj, GX_TEXMAP0);
}

class ParryBarDraw : public dDlst_base_c {
public:
    virtual void draw() {
        if (g_bars.empty()) return;

        Mtx44 proj;
        C_MTXOrtho(proj, 0.0f, 448.0f, 0.0f, 608.0f, 0.0f, 10.0f);
        GXSetProjection(proj, GX_ORTHOGRAPHIC);
        Mtx ident;
        PSMTXIdentity(ident);
        GXLoadPosMtxImm(ident, GX_PNMTX0);
        GXSetCurrentMtx(GX_PNMTX0);

        GXClearVtxDesc();
        GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
        GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
        GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
        GXSetNumChans(1);
        GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
        GXSetNumTexGens(1);
        GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        GXSetNumTevStages(1);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        GXSetZMode(GX_DISABLE, GX_ALWAYS, GX_DISABLE);
        GXSetCullMode(GX_CULL_NONE);

        const float barH = BAR_WIDTH * (float)BAR_TEX_H / (float)BAR_TEX_W;
        GXTexObj texEmpty, texFull;

        for (const BarInfo& bar : g_bars) {
            float left = bar.x - BAR_WIDTH * 0.5f;
            float top = bar.y - barH * 0.5f;

            // Barra vacia completa
            bind_texture(&texEmpty, TEX_BAR_EMPTY);
            draw_quad(left, top, left + BAR_WIDTH, top + barH, 0.0f, 1.0f, 255);

            // Relleno: solo la parte interior, hasta el porcentaje actual
            if (bar.ratio > 0.005f) {
                float u1 = FILL_U0 + (FILL_U1 - FILL_U0) * bar.ratio;
                bind_texture(&texFull, TEX_BAR_FULL);
                draw_quad(left + BAR_WIDTH * FILL_U0, top,
                          left + BAR_WIDTH * u1, top + barH,
                          FILL_U0, u1, 255);
            }
        }
    }
};

static ParryBarDraw g_barDraw;

// Despues de dibujar a Link: calcula donde va la barra del enemigo fijado.
static void on_link_draw_post(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    fopAc_ac_c* target = link->mTargetedActor;

    g_bars.clear();

    // Los enemigos que ya no estan fijados reinician su suavizado de posicion.
    uint32_t targetId = target ? (uint32_t)fopAcM_GetID(target) : 0;
    for (auto& kv : g_enemies) {
        if (!target || kv.first != targetId) kv.second.hasScreen = false;
    }
    if (!target) return;

    EnemyState& st = g_enemies[targetId];

    // Altura de la cabeza suavizada (la animacion mueve eyePos y haria temblar la barra)
    float h = target->eyePos.y - target->current.pos.y;
    if (!st.hasScreen) st.heightOff = h;
    else st.heightOff += (h - st.heightOff) * 0.10f;

    cXyz p(target->current.pos.x,
           target->current.pos.y + st.heightOff + BAR_HEIGHT_ABOVE_HEAD,
           target->current.pos.z);
    Vec s;
    mDoLib_project(&p, &s);
    if (s.x < 0.0f || s.x > 608.0f || s.y < 0.0f || s.y > 448.0f) {
        st.hasScreen = false;
        return;
    }

    // Suavizado de la posicion en pantalla
    if (!st.hasScreen) {
        st.sx = s.x;
        st.sy = s.y;
        st.hasScreen = true;
    } else {
        st.sx += (s.x - st.sx) * POS_SMOOTHING;
        st.sy += (s.y - st.sy) * POS_SMOOTHING;
    }

    // Relleno animado hacia el valor real
    bool exposed = st.stunTimer > 0 || st.secondTimer > 0 || st.holdTimer > 0;
    float goal = exposed ? 1.0f : (float)st.parries / (float)PARRIES_TO_STUN;
    st.dispRatio += (goal - st.dispRatio) * FILL_SMOOTHING;
    if (std::fabs(goal - st.dispRatio) < 0.003f) st.dispRatio = goal;

    g_bars.push_back({st.sx, st.sy, st.dispRatio});
    dComIfGd_set2DXlu(&g_barDraw);
}

#endif  // ENABLE_BAR

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult r;
    if ((r = mods::hook::add_post<GuardAttackInit>(on_bash_post)) != MOD_OK)
        return mods::set_error(error, r, "hook empuje de escudo");
    if ((r = mods::hook::add_pre<GuardSe>(on_guard_se_pre)) != MOD_OK)
        return mods::set_error(error, r, "hook golpe en escudo");
    if ((r = mods::hook::add_pre<GuardSlipInit>(skip_if_parry)) != MOD_OK)
        return mods::set_error(error, r, "hook retroceso");
    if ((r = mods::hook::add_pre<GuardBreakInit>(skip_if_parry)) != MOD_OK)
        return mods::set_error(error, r, "hook guardia rota");
    if ((r = mods::hook::add_pre<SmallGuard>(skip_if_parry)) != MOD_OK)
        return mods::set_error(error, r, "hook guardia chica");
    if ((r = mods::hook::add_post<ShieldGuard>(on_shield_guard_post)) != MOD_OK)
        return mods::set_error(error, r, "hook escudo automatico");
    if ((r = mods::hook::add_pre<CutNormalInit>(block_attack_int)) != MOD_OK)
        return mods::set_error(error, r, "hook ataque normal");
    if ((r = mods::hook::add_pre<CutFinishInit>(block_attack_int)) != MOD_OK)
        return mods::set_error(error, r, "hook tajo final");
    if ((r = mods::hook::add_pre<CutJumpInit>(block_attack_int)) != MOD_OK)
        return mods::set_error(error, r, "hook tajo con salto");
    if ((r = mods::hook::add_pre<CutTurnInit>(block_attack_int)) != MOD_OK)
        return mods::set_error(error, r, "hook tajo giratorio");
    if ((r = mods::hook::add_pre<CutTurnChargeInit>(block_attack_int)) != MOD_OK)
        return mods::set_error(error, r, "hook carga giratoria");
    if ((r = mods::hook::add_pre<CutDownInit>(block_attack_int)) != MOD_OK)
        return mods::set_error(error, r, "hook golpe final");
    if ((r = mods::hook::add_pre<CutHeadInit>(block_attack_int)) != MOD_OK)
        return mods::set_error(error, r, "hook tajo de casco");
    if ((r = mods::hook::add_pre<CutLargeJumpChargeInit>(block_attack_int)) != MOD_OK)
        return mods::set_error(error, r, "hook carga gran giro");
    if ((r = mods::hook::add_pre<CutLargeJumpInit>(block_attack_int)) != MOD_OK)
        return mods::set_error(error, r, "hook gran giro");
    if ((r = mods::hook::add_pre<CutDash>(block_attack_void)) != MOD_OK)
        return mods::set_error(error, r, "hook ataque en carrera");
    if ((r = mods::hook::add_pre<LinkExecute>(on_execute_pre)) != MOD_OK)
        return mods::set_error(error, r, "hook Link execute");
    if ((r = mods::hook::add_post<LinkExecute>(on_execute_post)) != MOD_OK)
        return mods::set_error(error, r, "hook Link execute post");
#if ENABLE_BAR
    if ((r = mods::hook::add_post<LinkDraw>(on_link_draw_post)) != MOD_OK)
        return mods::set_error(error, r, "hook dibujo de Link");
#endif
#if SOUND_LOG
    if (mods::hook::add_pre<SeStartLog>(on_se_start_log) != MOD_OK)
        svc_log->warn(mod_ctx, "[AUDIO] Error: no pude hookear Z2SeMgr::seStart");
    else
        svc_log->info(mod_ctx, "[AUDIO] *** SISTEMA DE LOG DE SONIDOS ACTIVADO ***");
#endif
#if AUDIO_REPLACE
    if (svc_audio_res != nullptr) {
        AudioWaveHandle waveHandle = 0;
        ModResult ar = svc_audio_res->replace_wave(
            mod_ctx, PARRY_WAVE_BANK, PARRY_WAVE_ID, "res/parry_success.wav", nullptr, &waveHandle);
        if (ar != MOD_OK) svc_log->warn(mod_ctx, "AUDIO: no pude reemplazar la muestra de sonido");
        else svc_log->info(mod_ctx, "AUDIO: muestra reemplazada");
    } else {
        svc_log->warn(mod_ctx, "AUDIO: esta version de Dusklight no tiene el servicio de audio");
    }
#endif
#if AUDIO_SCAN
    // Si alguno falla, solo avisa: el resto del mod sigue funcionando.
    if (mods::hook::add_pre<ScanBasicWave>(on_scan_wave) != MOD_OK)
        svc_log->warn(mod_ctx, "SCAN: no pude hookear JASBasicWaveBank");
    if (mods::hook::add_pre<ScanSimpleWave>(on_scan_wave) != MOD_OK)
        svc_log->warn(mod_ctx, "SCAN: no pude hookear JASSimpleWaveBank");
#endif
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {   // se llama cada frame
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    // El loader quita los hooks solo al desactivar el mod.
    g_enemies.clear();
#if ENABLE_BAR
    g_bars.clear();
#endif
    g_parryTimer = 0;
    g_parryHitThisTick = false;
    g_attackLock = 0;
    g_bypassLock = false;
    return MOD_OK;
}

}
