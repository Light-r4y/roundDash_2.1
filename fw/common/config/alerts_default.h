#pragma once

// Правила тревог по умолчанию (формат - alerts.h). Действуют, пока в NVS нет сохранённых; вернуть - кнопка на
// странице «Звук» или POST /api/sound/rules/reset. Сам звук включается режимом sound.mode (по умолчанию выключен).
static const char RG_ALERTS_DEFAULT_JSON[] =
    "{\"version\":1,\"rules\":["
    "{\"signal\":\"coolant\",\"op\":\">\",\"value\":105,\"hyst\":3,\"pattern\":\"beeps\",\"count\":3,\"on\":120,\"off\":120,\"repeat\":5,\"enabled\":true},"
    "{\"signal\":\"rpm\",\"op\":\">\",\"value\":7000,\"hyst\":100,\"pattern\":\"beeps\",\"count\":2,\"on\":80,\"off\":80,\"repeat\":0,\"enabled\":true}]}";
