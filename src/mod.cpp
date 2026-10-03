// Mod para reproducir sonidos por ID al presionar L (fijar objetivo)
// Versión corregida: no llama a un método no-estático como si fuese static

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

// Hook definitions (captura de sonidos y tick de Link)
DEFINE_HOOK(&Z2SeMgr::seStart, SeStartLog);
DEFINE_HOOK(&daAlink_c::execute, LinkExecute);

// --- Tabla de nombres para logs ---
static std::map<uint32_t, std::string> g_soundNames = {
    {0x0400000A, "LINK_JUMP"},
    {0x04000014, "LINK_LAND"},
    {0x04000015, "LINK_LAND_HEAVY"},
    {0x04000016, "LINK_HURT"},
    // ...
};

static std::map<uint32_t, int> g_soundCount;
static const int MAX_REPEAT_LOG = 3;

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

// Hook: captura todos los sonidos reproducidos
static HookAction on_se_start_log(ModContext* ctx, void* args, void*, void*) {
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

// --- Función de reproducción corregida: requiere una instancia real de Z2SeMgr ---
static void playSeById(Z2SeMgr* seMgr, uint32_t id) {
    if (seMgr == nullptr) {
        char b[128];
        snprintf(b, sizeof(b), "[PLAY] instancia Z2SeMgr no disponible; ID pendiente: 0x%08X", id);
        svc_log->info(mod_ctx, b);
        return;
    }

    // Este es el uso correcto del método no-estático:
    seMgr->seStart(id, nullptr, 0, 0, 0, 1.0f);

    char b[80];
    snprintf(b, sizeof(b), "[PLAY] solicitada reproduccion ID: 0x%08X", id);
    svc_log->info(mod_ctx, b);
}

// --- Detección de tecla L para reproducir los dos IDs del ejemplo ---
static bool g_wasLDown = false;

static HookAction on_execute_pre(ModContext* ctx, void* args, void*, void*) {
    static int tickCounter = 0;
    tickCounter++;
    if (tickCounter >= 1800) {
        tickCounter = 0;
        for (auto &kv : g_soundCount) {
            if (kv.second > MAX_REPEAT_LOG) kv.second = MAX_REPEAT_LOG;
        }
    }

#ifdef _WIN32
    SHORT keyState = GetAsyncKeyState('L');
    bool isDown = (keyState & 0x8000) != 0;
    if (isDown && !g_wasLDown) {
        g_wasLDown = true;

        // Importante: esto NO llama a un método static; requiere una instancia real.
        // En esta compilación no hay instancia conocida del port, así que solo se loguea.
        playSeById(nullptr, 0x00000058);
        playSeById(nullptr, 0x0000005A);
    } else if (!isDown) {
        g_wasLDown = false;
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
    svc_log->info(mod_ctx, "MOD DE REPRODUCCION: pulsar 'L' captura IDs 0x58 y 0x5A");
    svc_log->info(mod_ctx, "El método Z2SeMgr::seStart requiere instancia real del audio manager");
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
