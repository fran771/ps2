#ifndef WARESDES_PS2_H
#define WARESDES_PS2_H

/* Waresdes PS/2 driver - bare metal, polling mode.
   Compile ps2.c together with your kernel. No libc or external headers required. */

typedef enum {
    ABNT2 = 0,
    US   = 1,
    ABNT = 2
} PS2KeyboardLayout;

typedef struct {
    int CTRL_ON;
    int SHIFT_ON;
    int CAPS_ON;
    int ALT_ON;
    int ALTGR_ON;
} PS2Modifiers;

typedef struct {
    int keyPress;       /* 1 = tecla pressionada; 0 = soltura/nenhum evento */
    unsigned char asciiKey; /* 0 para teclas sem caractere ASCII */
    unsigned short ps2key;   /* scancode Set 1: E0xx para teclas estendidas */
    unsigned char keyDown;   /* 1 = make; 0 = break */
    PS2Modifiers mod;
} PS2KeyboardInfo;

typedef struct {
    int x;              /* movimento acumulado desde GetMouseInfo */
    int y;
    int scroll;         /* positivo = roda para cima; negativo = para baixo */
    unsigned char left;
    unsigned char right;
    unsigned char middle;
    unsigned char packetReady;
} PS2MouseInfo;

/* Inicializa o controlador, teclado e mouse. Retorna 1 em sucesso, 0 em falha. */
int InitPS2(void);

/* Lê todos os bytes PS/2 disponíveis e atualiza os pacotes de teclado/mouse.
   Chame frequentemente no loop principal do kernel. */
void ReadPS2(void);

/* Copia os dados mais recentes. Mouse x/y/scroll são consumidos ao copiar.
   keyPress é consumido ao copiar; modificadores continuam atualizados. */
void GetMouseInfo(PS2MouseInfo *mouse);
void GetKeyboardInfo(PS2KeyboardInfo *kbd);

/* Troca o mapa lógico do teclado em tempo de execução. */
void LayoutKbdPS2(PS2KeyboardLayout layout);

/* Estado atual dos modificadores, caso queira consultar sem consumir evento. */
void GetPS2Modifiers(PS2Modifiers *mods);

/* Diagnóstico simples. */
int PS2IsInitialized(void);

#endif
