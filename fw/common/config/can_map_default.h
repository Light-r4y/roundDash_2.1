#pragma once

// Привязки по умолчанию - пресет rusEFI: кадры 0x200..0x207 (базовый ID 0x200, 500 кбит/с), все поля Intel (@1).
// Формат и поля - can_map.h. Действует, пока в NVS нет сохранённой таблицы; вернуть - кнопка на странице CAN
// или POST /api/can/map/reset. Имена сигналов должны быть в таблице сигналов встроенной раскладки.
static const char RG_CAN_MAP_DEFAULT_JSON[] =
    "{\"version\":1,\"map\":[{\"signal\":\"rpm\",\"id\":\"0x201\",\"start\":0,\"len\":16,\"order\":\"intel\"},"
    "{\"signal\":\"speed\",\"id\":\"0x201\",\"start\":48,\"len\":8,\"order\":\"intel\"},"
    "{\"signal\":\"coolant\",\"id\":\"0x203\",\"start\":16,\"len\":8,\"order\":\"intel\",\"offset\":-40},"
    "{\"signal\":\"iat\",\"id\":\"0x203\",\"start\":24,\"len\":8,\"order\":\"intel\",\"offset\":-40},"
    "{\"signal\":\"map\",\"id\":\"0x203\",\"start\":0,\"len\":16,\"order\":\"intel\",\"factor\":0.03333333},"
    "{\"signal\":\"fuel_level\",\"id\":\"0x203\",\"start\":56,\"len\":8,\"order\":\"intel\",\"factor\":0.5},"
    "{\"signal\":\"oil_press\",\"id\":\"0x204\",\"start\":16,\"len\":16,\"order\":\"intel\",\"factor\":0.0003333333},"
    "{\"signal\":\"oil_temp\",\"id\":\"0x204\",\"start\":32,\"len\":8,\"order\":\"intel\",\"offset\":-40},"
    "{\"signal\":\"vbat\",\"id\":\"0x204\",\"start\":48,\"len\":16,\"order\":\"intel\",\"factor\":0.001},"
    "{\"signal\":\"lambda\",\"id\":\"0x207\",\"start\":0,\"len\":16,\"order\":\"intel\",\"factor\":0.0001},"
    "{\"signal\":\"tps\",\"id\":\"0x202\",\"start\":16,\"len\":16,\"order\":\"intel\",\"signed\":true,\"factor\":0.01},"
    "{\"signal\":\"afr\",\"id\":\"0x207\",\"start\":0,\"len\":16,\"order\":\"intel\",\"factor\":0.00147}]}";
