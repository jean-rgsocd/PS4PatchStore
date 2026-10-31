/* Interface do app (independente de plataforma): desenha em uma superficie Image e recebe teclas logicas. */
#ifndef UI_H
#define UI_H

#include "gfx.h"
#include "catalog.h"

enum { UI_UP = 1, UI_DOWN, UI_LEFT, UI_RIGHT, UI_OK, UI_BACK, UI_L1, UI_R1, UI_OPTIONS };
enum { UI_TOAST_INFO = 0, UI_TOAST_OK = 1, UI_TOAST_ERR = 2 };

typedef struct UI UI;
/* Chamada quando o usuario pede uma instalacao. Deve retornar rapido (a instalacao roda em outra thread). */
typedef void (*UiInstallFn)(const CatButton *b, void *user);

UI  *ui_create(Image *surface, const Catalog *cat, const char *assets_dir, UiInstallFn fn, void *user);
void ui_key(UI *ui, int key);
void ui_tick(UI *ui);                 /* chamar ~60x por segundo */
int  ui_take_dirty(UI *ui);           /* 1 se precisa redesenhar (e zera a flag) */
void ui_draw(UI *ui);
void ui_set_busy(UI *ui, int busy);
void ui_toast(UI *ui, const char *msg, int kind);

#endif
