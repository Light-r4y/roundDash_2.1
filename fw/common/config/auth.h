#pragma once
#include <stdbool.h>

// Необязательный пароль, защищающий OTA и изменяющие (POST) запросы API - чтение
// состояния (GET) остаётся открытым. Хранится в NVS как SHA-256(соль + пароль)
// (см. conf.h: RG_AUTH_SALT), пустой/отсутствующий хэш означает "защита выключена"
// (поведение по умолчанию).
//
// Сброс забытого пароля: удержание кнопки 1 на RG_AUTH_RESET_HOLD_MS (buttons_task) на
// работающей плате; при включении кнопку держать нельзя - это GPIO0 (BOOT). Консоли для сброса
// нет: RX UART0 (GPIO44) занят кнопкой 2, USB-Serial/JTAG отдан CAN. Новый пароль задаётся на
// странице "Доступ" (POST /api/auth/password).
void roundGauge_auth_init(void);

// Включена ли защита сейчас (задан ли пароль).
bool roundGauge_auth_is_enabled(void);

// Задать новый пароль (сохраняется в NVS) либо снять защиту (password == "" или NULL).
bool roundGauge_auth_set_password(const char *password);

// Проверить пароль против сохранённого хэша. Если защита выключена - всегда true.
bool roundGauge_auth_check(const char *password);
