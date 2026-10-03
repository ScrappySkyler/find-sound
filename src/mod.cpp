// Mod para reproducir sonidos por ID al presionar L (fijar objetivo)
// Reescritura completa de src/mod.cpp según petición

#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <string>
#include <map>

#include "mods/service.hpp"
#include "mods/svc/log.h"
#include "mods/svc/hook.hpp"

#include "d/actor/d_a_alink.h"
#include "d/d_com_inf_game.h"
#include "f_op/f_op_actor_mng.h"
#include "m_Do/m_Do_controller_pad.h"
#include "m_Do/m_Do_audio.h"
#include "SSystem/SComponent/c_math.h"

#include "Z2AudioLib/Z2SeMgr.h"

#ifdef _WIN32
  #include <Windows.h>
#endif

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);

// Hook definitions (se usan para monitorizar y seguir capturando sonidos como antes)
DEFINE_HOOK(&Z2SeMgr::seStart, SeStartLog);
DEFINE_HOOK(&daAlink_c::execute, LinkExecute);

// --- Tabla de nombres (útil para logs) ---
static std::map<uint32_t, std::string> g_soundNames = {
    {0x0400000A, "LINK_JUMP"},
    {0x04000014, "LINK_LAND"},
    {0x04000015, "LINK_LAND_HEAVY"},
    {0x04000016, "LINK_HURT"},
    // ... (se pueden ampliar según sea necesario)
};

static std::map<uint32_t, int> g_soundCount;
static const int MAX_REPEAT_LOG = 3; // limitar logs repetidos

static std::string get_sound_name(uint32_t soundId) {
    auto it = g_soundNames.find(soundId);
    if (it != g_soundNames.end()) return it->second;

    uint8_t section = (soundId >> 24) & 0xFF;
    uint8_t group = (soundId >> 16) & 0xFF;
    uint16_t id = soundId & 0xFFFF;

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "UNKNOWN[Sec:%u|Grp:%u|ID:%u]", section, group, id);
    return std::string(buffer);
}

static uint32_t swap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

// --- Hook: capturar todos los sonidos (log) ---
static HookAction on_se_start_log(ModContext* ctx, void* args, void*, void*) {
    // El segundo argumento (index 1) contiene el ID del SE en muchos ports
    uint32_t rawId = mods::arg<uint32_t>(args, 1);
    uint32_t seId = swap32(rawId);

    int& count = g_soundCount[seId];
    count++;
    if (count > MAX_REPEAT_LOG) return HOOK_CONTINUE;

    std::string name = get_sound_name(seId);
    char msg[256];
    if (count > 1) {
        snprintf(msg, sizeof(msg), "[SONIDO] %s (ID: 0x%08X) [REPETICION %d]", name.c_str(), seId, count);
    } else {
        snprintf(msg, sizeof(msg), "[SONIDO] %s (ID: 0x%08X)", name.c_str(), seId);
    }
    svc_log->info(ctx, msg);

    return HOOK_CONTINUE;
}

// --- Función para reproducir un SE por ID ---
static void playSeById(uint32_t id) {
    // swap para ajustar endianness si el engine lo requiere
    uint32_t seId = swap32(id);

    // Intenta llamar a Z2SeMgr::seStart
    // Firma típica (según ingeniería inversa): seStart(unsigned long soundId, Vec* pos, int, int, int, float)
    // Pasamos NULL para posición y parámetros por defecto.
    try {
        // Si la función está expuesta como estática/miembro accesible:
        Z2SeMgr::seStart(seId, nullptr, 0, 0, 0, 1.0f);

        // Log auxiliar
        char b[80];
        snprintf(b, sizeof(b), "[PLAY] solicitada reproduccion ID: 0x%08X", id);
        svc_log->info(mod_ctx, b);
    } catch(...) {
        // Si no es posible llamar directamente, logueamos para depuración
        char b[120];
        snprintf(b, sizeof(b), "[PLAY] ERROR al intentar reproducir ID: 0x%08X (llamada directa fallida)", id);
        svc_log->info(mod_ctx, b);
    }
}

// --- Detección de tecla L para reproducir los dos IDs (88 y 90) ---
static bool g_wasLDown = false;

static HookAction on_execute_pre(ModContext* ctx, void* args, void*, void*) {
    // Lógica de limpieza de logs repetidos (mantener similar a implementaciones previas)
    static int tickCounter = 0;
    tickCounter++;
    if (tickCounter >= 1800) { // cada ~60s a 30Hz
        tickCounter = 0;
        for (auto &kv : g_soundCount) {
            if (kv.second > MAX_REPEAT_LOG) kv.second = MAX_REPEAT_LOG;
        }
    }

    // Detectar pulsación de tecla L (Windows)
#ifdef _WIN32
    SHORT keyState = GetAsyncKeyState('L');
    bool isDown = (keyState & 0x8000) != 0;
    if (isDown && !g_wasLDown) {
        g_wasLDown = true;
        // Reproducir los dos IDs que indicaste: 0x58 (88) y 0x5A (90)
        playSeById(0x00000058);
        playSeById(0x0000005A);
    } else if (!isDown) {
        g_wasLDown = false;
    }
#endif

    // También se puede intentar detectar el botón de fijar objetivo del pad (si la API lo permite)
    // Ejemplo simple (dependiente del port) - no compilará si la API difiere:
#if 0
    // Descomenta y adapta si conoces la función exacta para leer el botón de fijar objetivo
    if (mDoControllerPad_checkLockOn()) {
        // lógica...
    }
#endif

    return HOOK_CONTINUE;
}

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult r;

    if ((r = mods::hook::add_pre<SeStartLog>(on_se_start_log)) != MOD_OK) {
        return mods::set_error(error, r, "No se pudo hookear Z2SeMgr::seStart");
    }

    if ((r = mods::hook::add_pre<LinkExecute>(on_execute_pre)) != MOD_OK) {
        return mods::set_error(error, r, "No se pudo hookear daAlink_c::execute");
    }

    svc_log->info(mod_ctx, "========================================");
    svc_log->info(mod_ctx, "MOD DE REPRODUCCION: pulsa 'L' para reproducir IDs 0x58 y 0x5A");
    svc_log->info(mod_ctx, "========================================");

    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    g_soundCount.clear();
    svc_log->info(mod_ctx, "[PLAY] Mod detenido, registro limpiado");
    return MOD_OK;
}

}
