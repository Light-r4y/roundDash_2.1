#pragma once
#include <stdbool.h>

// Необязательный пароль, защищающий OTA и изменяющие (POST) запросы API - чтение
// состояния (GET) остаётся открытым. Хранится в NVS как SHA-256(соль + пароль)
// (см. conf.h: RG_AUTH_SALT), пустой/отсутствующий хэш означает "защита выключена"
// (поведение по умолчанию).
//
// Перенесено из wifi-serv (src/config/auth.c) с заменой префиксов.
//
// Сброс забытого пароля. В wifi-serv это команда в serial-консоли, здесь так нельзя:
// RX UART0 (GPIO44) занят кнопкой 2, USB-Serial/JTAG — под CAN. Поэтому пароль снимает
// удержание кнопки 1 на RG_AUTH_RESET_HOLD_MS (buttons_task); при включении кнопку
// держать нельзя - это GPIO0 (BOOT). Задать новый пароль можно с главной страницы веба
// (POST /api/auth/password).
void roundGauge_auth_init(void);

// Включена ли защита сейчас (задан ли пароль).
bool roundGauge_auth_is_enabled(void);

// Задать новый пароль (сохраняется в NVS) либо снять защиту (password == "" или NULL).
bool roundGauge_auth_set_password(const char *password);

// Проверить пароль против сохранённого хэша. Если защита выключена - всегда true.
bool roundGauge_auth_check(const char *password);
