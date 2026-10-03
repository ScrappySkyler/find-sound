// Mod para encontrar nombres de sonidos en Dusklight
// Muestra en tiempo real el nombre descriptivo de cada sonido reproducido
// Usa el sistema de log de Dusklight para capturar y traducir IDs de audio

#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <cmath>
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

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);

DEFINE_HOOK(&Z2SeMgr::seStart, SeStartLog);
DEFINE_HOOK(&daAlink_c::execute, LinkExecute);

// ========== TABLA DE NOMBRES DE SONIDOS ==========
// Mapeo de IDs de sonido a nombres descriptivos
static std::map<uint32_t, std::string> g_soundNames = {
    // Sonidos de Link / Acciones
    {0x0400000A, "LINK_JUMP"},
    {0x04000014, "LINK_LAND"},
    {0x04000015, "LINK_LAND_HEAVY"},
    {0x04000016, "LINK_HURT"},
    {0x04000017, "LINK_GRUNT"},
    {0x04000018, "LINK_BREAK_FREE"},
    {0x04000019, "LINK_GASP"},
    {0x0400001A, "LINK_YELL"},
    {0x0400001B, "LINK_LAUGH"},
    {0x0400001C, "LINK_WAIL"},
    {0x0400001D, "LINK_KNIFE_THROW"},
    
    // Sonidos de guardia / defensa
    {0x04000020, "GUARD_HIT"},
    {0x04000021, "GUARD_BREAK"},
    {0x04000022, "GUARD_BLOCK"},
    {0x04000023, "MIDNA_JUMP"},
    {0x04000024, "TITLE_ENTER"},
    
    // Ataques con espada
    {0x04000030, "SWORD_SWING_1"},
    {0x04000031, "SWORD_SWING_2"},
    {0x04000032, "SWORD_SWING_3"},
    {0x04000033, "SWORD_CLASH"},
    {0x04000034, "SWORD_PIERCE"},
    {0x04000035, "SWORD_HEAVY_HIT"},
    
    // Sonidos de arco
    {0x04000040, "BOW_DRAW"},
    {0x04000041, "BOW_FIRE"},
    {0x04000042, "ARROW_HIT"},
    {0x04000043, "ARROW_RICOCHET"},
    
    // Sonidos de magia / poderes especiales
    {0x04000050, "MAGIC_CAST"},
    {0x04000051, "MAGIC_HIT"},
    {0x04000052, "MAGIC_FAIL"},
    {0x04000053, "BOMB_THROW"},
    {0x04000054, "BOMB_EXPLODE"},
    
    // UI / Menu
    {0x04000060, "MENU_OPEN"},
    {0x04000061, "MENU_CLOSE"},
    {0x04000062, "MENU_SELECT"},
    {0x04000063, "MENU_ERROR"},
    {0x04000064, "ITEM_GET"},
    {0x04000065, "ITEM_DROP"},
    
    // Ambiente
    {0x04000070, "WIND"},
    {0x04000071, "RAIN"},
    {0x04000072, "THUNDER"},
    {0x04000073, "BIRD"},
    {0x04000074, "WATER"},
    {0x04000075, "FOOTSTEP"},
    {0x04000076, "FOOTSTEP_WATER"},
    {0x04000077, "FOOTSTEP_GRASS"},
    
    // Enemigos generales
    {0x04000080, "ENEMY_SPAWN"},
    {0x04000081, "ENEMY_ALERT"},
    {0x04000082, "ENEMY_ATTACK"},
    {0x04000083, "ENEMY_HIT"},
    {0x04000084, "ENEMY_DEATH"},
    {0x04000085, "ENEMY_ROAR"},
    
    // Jefes
    {0x04000090, "BOSS_APPEAR"},
    {0x04000091, "BOSS_ATTACK"},
    {0x04000092, "BOSS_ROAR"},
    {0x04000093, "BOSS_WEAK"},
    {0x04000094, "BOSS_DEFEATED"},
    
    // Puertas y mecanismos
    {0x040000A0, "DOOR_OPEN"},
    {0x040000A1, "DOOR_CLOSE"},
    {0x040000A2, "DOOR_LOCK"},
    {0x040000A3, "DOOR_UNLOCK"},
    {0x040000A4, "CHEST_OPEN"},
    {0x040000A5, "MECHANISM_ACTIVATE"},
    {0x040000A6, "MECHANISM_CLICK"},
    
    // Vibraciones / Efectos de impacto
    {0x040000B0, "IMPACT_SOFT"},
    {0x040000B1, "IMPACT_MEDIUM"},
    {0x040000B2, "IMPACT_HEAVY"},
    {0x040000B3, "HIT_FLESH"},
    {0x040000B4, "HIT_METAL"},
    {0x040000B5, "HIT_WOOD"},
    {0x040000B6, "HIT_STONE"},
};

// Registro de sonidos ya vistos para evitar spam
static std::map<uint32_t, int> g_soundCount;
static const int MAX_REPEAT_LOG = 3;  // Solo mostrar las primeras 3 repeticiones

// Función para obtener el nombre de un sonido
static std::string get_sound_name(uint32_t soundId) {
    auto it = g_soundNames.find(soundId);
    if (it != g_soundNames.end()) {
        return it->second;
    }
    
    // Decodificar componentes del ID para nombres más descriptivos
    uint8_t section = (soundId >> 24) & 0xFF;
    uint8_t group = (soundId >> 16) & 0xFF;
    uint16_t id = soundId & 0xFFFF;
    
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "UNKNOWN[Sec:%u|Grp:%u|ID:%u]", 
             section, group, id);
    return std::string(buffer);
}

// Función para convertir uint32_t a formato legible
static uint32_t swap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
}

// Hook principal: captura TODOS los sonidos reproducidos
static HookAction on_se_start_log(ModContext*, void* args, void*, void*) {
    uint32_t seId = swap32(mods::arg<uint32_t>(args, 1));
    
    // Contar repeticiones
    int& count = g_soundCount[seId];
    count++;
    
    // Solo loguear las primeras 3 repeticiones de cada sonido
    if (count > MAX_REPEAT_LOG) {
        return HOOK_CONTINUE;
    }
    
    // Obtener nombre del sonido
    std::string soundName = get_sound_name(seId);
    
    // Formatear el mensaje de log
    char logMessage[512];
    if (count > 1) {
        snprintf(logMessage, sizeof(logMessage),
                 "[SONIDO] %s (ID: 0x%08X | Hex: %02X-%02X-%04X) [REPETICION %d]",
                 soundName.c_str(), seId,
                 (seId >> 24) & 0xFF, (seId >> 16) & 0xFF, seId & 0xFFFF,
                 count);
    } else {
        snprintf(logMessage, sizeof(logMessage),
                 "[SONIDO] %s (ID: 0x%08X | Hex: %02X-%02X-%04X)",
                 soundName.c_str(), seId,
                 (seId >> 24) & 0xFF, (seId >> 16) & 0xFF, seId & 0xFFFF);
    }
    
    svc_log->info(mod_ctx, logMessage);
    
    return HOOK_CONTINUE;
}

// Cada tick: limpiar el registro si es necesario
static int g_tickCounter = 0;
static const int RESET_EVERY_TICKS = 1800;  // Limpiar cada 60 segundos (1800 ticks a 30/s)

static HookAction on_execute_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    
    g_tickCounter++;
    
    // Limpiar el registro cada cierto tiempo para evitar memoria infinita
    if (g_tickCounter >= RESET_EVERY_TICKS) {
        g_tickCounter = 0;
        
        // Reducir contador de repeticiones (permite ver el sonido nuevamente)
        for (auto& kv : g_soundCount) {
            if (kv.second > MAX_REPEAT_LOG) {
                kv.second = MAX_REPEAT_LOG;
            }
        }
        
        // Limpiar completamente cada 10 minutos
        static int fullResetCounter = 0;
        if (++fullResetCounter >= 10) {
            fullResetCounter = 0;
            g_soundCount.clear();
            svc_log->info(mod_ctx, "[SONIDOS] *** Registro limpiado ***");
        }
    }
    
    return HOOK_CONTINUE;
}

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    ModResult r;
    
    // Hookear el sistema de audio
    if ((r = mods::hook::add_pre<SeStartLog>(on_se_start_log)) != MOD_OK) {
        return mods::set_error(error, r, "No se pudo hookear Z2SeMgr::seStart");
    }
    
    // Hookear el execute de Link para limpiar el registro
    if ((r = mods::hook::add_pre<LinkExecute>(on_execute_pre)) != MOD_OK) {
        return mods::set_error(error, r, "No se pudo hookear daAlink_c::execute");
    }
    
    svc_log->info(mod_ctx, "========================================");
    svc_log->info(mod_ctx, "SISTEMA DE BÚSQUEDA DE SONIDOS ACTIVADO");
    svc_log->info(mod_ctx, "========================================");
    svc_log->info(mod_ctx, "Abre la consola del mod para ver cada sonido");
    svc_log->info(mod_ctx, "reproducido en tiempo real con su nombre e ID");
    svc_log->info(mod_ctx, "Formato: [SONIDO] NOMBRE (ID: 0xXXXXXXXX)");
    svc_log->info(mod_ctx, "========================================");
    
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    g_soundCount.clear();
    g_tickCounter = 0;
    svc_log->info(mod_ctx, "[SONIDOS] Mod desactivado - Registro limpiado");
    return MOD_OK;
}

}
