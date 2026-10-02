#pragma once
#include <stdbool.h>

// Необязательный пароль, защищающий OTA и изменяющие (POST) запросы API - чтение
// состояния (GET) остаётся открытым. Хранится в NVS как SHA-256(соль + пароль)
// (см. conf.h: RG_AUTH_SALT), пустой/отсутствующий хэш означает "защита выключена"
// (поведение по умолчанию).
//
// Перенесено из wifi-serv (src/config/auth.c) с заменой префиксов.
//
// TODO: сброс забытого пароля. В wifi-serv это команда в serial-консоли, здесь
// так нельзя: RX UART0 (GPIO44) занят кнопкой 2, USB-Serial/JTAG — под CAN.
// Способ не выбран (docs/fw-design.md, "Открытые вопросы").
void roundGauge_auth_init(void);

// Включена ли защита сейчас (задан ли пароль).
bool roundGauge_auth_is_enabled(void);

// Задать новый пароль (сохраняется в NVS) либо снять защиту (password == "" или NULL).
bool roundGauge_auth_set_password(const char *password);

// Проверить пароль против сохранённого хэша. Если защита выключена - всегда true.
bool roundGauge_auth_check(const char *password);
