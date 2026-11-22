#include "ps2.h"

/* Portas padrão do controlador i8042. */
#define PS2_DATA       0x60
#define PS2_STATUS     0x64
#define PS2_COMMAND    0x64

#define PS2_STATUS_OBF 0x01
#define PS2_STATUS_IBF 0x02
#define PS2_STATUS_AUX 0x20
#define PS2_STATUS_TIMEOUT 0x40
#define PS2_STATUS_PARITY  0x80

/* I/O inline: substitua somente se o seu kernel já possuir wrappers próprios. */
static inline unsigned char tinb(unsigned short port) {
    unsigned char value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void toutb(unsigned short port, unsigned char value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static int ps2_initialized = 0;
static PS2KeyboardLayout ps2_layout = ABNT2;
static PS2KeyboardInfo keyboard_info;
static PS2MouseInfo mouse_info;

static unsigned char key_down[256];
static unsigned char caps_lock = 0;
static unsigned char num_lock = 0;
static unsigned char scroll_lock = 0;
static unsigned char e0_prefix = 0;
static unsigned char pause_bytes = 0;

static unsigned char mouse_packet[4];
static unsigned char mouse_packet_pos = 0;
static unsigned char mouse_packet_size = 3;
static unsigned char mouse_id = 0;

/* Esperas limitadas: nunca deixa o kernel preso para sempre no controlador. */
static int wait_input_clear(void) {
    unsigned int i;
    for (i = 0; i < 100000; ++i)
        if (!(tinb(PS2_STATUS) & PS2_STATUS_IBF)) return 1;
    return 0;
}
static int wait_output_full(void) {
    unsigned int i;
    for (i = 0; i < 100000; ++i)
        if (tinb(PS2_STATUS) & PS2_STATUS_OBF) return 1;
    return 0;
}
static int controller_command(unsigned char command) {
    if (!wait_input_clear()) return 0;
    toutb(PS2_COMMAND, command);
    return 1;
}
static int controller_write_data(unsigned char value) {
    if (!wait_input_clear()) return 0;
    toutb(PS2_DATA, value);
    return 1;
}
static int controller_read_data(unsigned char *value) {
    if (!wait_output_full()) return 0;
    *value = tinb(PS2_DATA);
    return 1;
}

/* Envia um comando ao mouse pela porta auxiliar. */
static int mouse_write(unsigned char value) {
    unsigned char response;
    if (!controller_command(0xD4)) return 0;
    if (!controller_write_data(value)) return 0;
    if (!controller_read_data(&response)) return 0;
    if (response == 0xFA) return 1;
    /* Um RESEND é tentado uma vez. */
    if (response == 0xFE) {
        if (!controller_command(0xD4) || !controller_write_data(value)) return 0;
        if (!controller_read_data(&response)) return 0;
        return response == 0xFA;
    }
    return 0;
}

/* Envia comando ao teclado (porta principal). */
static int keyboard_write(unsigned char value) {
    unsigned char response;
    if (!controller_write_data(value) || !controller_read_data(&response)) return 0;
    if (response == 0xFA) return 1;
    if (response == 0xFE) {
        if (!controller_write_data(value) || !controller_read_data(&response)) return 0;
        return response == 0xFA;
    }
    return 0;
}

static void update_modifiers(void) {
    keyboard_info.mod.CTRL_ON  = key_down[0x1D] || key_down[0x9D];
    keyboard_info.mod.SHIFT_ON = key_down[0x2A] || key_down[0x36];
    keyboard_info.mod.ALT_ON   = key_down[0x38];
    keyboard_info.mod.ALTGR_ON = key_down[0xB8];
    keyboard_info.mod.CAPS_ON  = caps_lock;
}

/*
 * Tabelas de layout: cada índice é o scancode Set 1 (0x00..0x7F).
 * LOWER contém a tecla sem Shift; UPPER contém a tecla com Shift.
 * Para letras, CAPS é aplicado separadamente usando Shift XOR Caps.
 * Valores 0x80..0xFF são bytes estendidos (o kernel deve usar a mesma
 * página de códigos, por exemplo Latin-1, ao desenhar esses caracteres).
 */
#define KBD_MAP(name, ...) static const unsigned char name[128] = { __VA_ARGS__ }

/* ABNT2 brasileiro (teclado físico de  ABNT2 / scancode Set 1).
   Bytes 0x80..0xFF seguem Latin-1. Valores 0xF0..0xF3 são marcadores
   internos de teclas mortas e nunca são entregues diretamente ao usuário. */
#define DEAD_ACUTE     0xF0
#define DEAD_DIAERESIS 0xF1
#define DEAD_TILDE     0xF2
#define DEAD_CIRCUMFLEX 0xF3
#define DEAD_GRAVE     0xF4

KBD_MAP(kbd_abnt2_lower,
    [0x01]=27, [0x02]='1', [0x03]='2', [0x04]='3', [0x05]='4',
    [0x06]='5', [0x07]='6', [0x08]='7', [0x09]='8', [0x0A]='9',
    [0x0B]='0', [0x0C]='-', [0x0D]='=', [0x0E]=8, [0x0F]='\t',
    [0x10]='q', [0x11]='w', [0x12]='e', [0x13]='r', [0x14]='t',
    [0x15]='y', [0x16]='u', [0x17]='i', [0x18]='o', [0x19]='p',
    [0x1A]=DEAD_ACUTE, [0x1B]='[', [0x1C]='\n',
    [0x1E]='a', [0x1F]='s', [0x20]='d', [0x21]='f', [0x22]='g',
    [0x23]='h', [0x24]='j', [0x25]='k', [0x26]='l',
    [0x27]=135, [0x28]=DEAD_TILDE, [0x29]=39, [0x2B]=']',
    [0x2C]='z', [0x2D]='x', [0x2E]='c', [0x2F]='v', [0x30]='b',
    [0x31]='n', [0x32]='m', [0x33]=',', [0x34]='.', [0x35]=';',
    [0x37]='*', [0x39]=' ', [0x47]='7', [0x48]='8', [0x49]='9',
    [0x4A]='-', [0x4B]='4', [0x4C]='5', [0x4D]='6', [0x4E]='+',
    [0x4F]='1', [0x50]='2', [0x51]='3', [0x52]='0', [0x53]='.',
    [0x56]='<'
);
KBD_MAP(kbd_abnt2_upper,
    [0x01]=27, [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$',
    [0x06]='%', [0x07]=DEAD_DIAERESIS, [0x08]='&', [0x09]='*',
    [0x0A]='(', [0x0B]=')', [0x0C]='_', [0x0D]='+', [0x0E]=8,
    [0x0F]='\t', [0x10]='Q', [0x11]='W', [0x12]='E', [0x13]='R',
    [0x14]='T', [0x15]='Y', [0x16]='U', [0x17]='I', [0x18]='O',
    [0x19]='P', [0x1A]=DEAD_GRAVE, [0x1B]='{', [0x1C]='\n',
    [0x1E]='A', [0x1F]='S', [0x20]='D', [0x21]='F', [0x22]='G',
    [0x23]='H', [0x24]='J', [0x25]='K', [0x26]='L',
    [0x27]=128, [0x28]=DEAD_CIRCUMFLEX, [0x29]='"', [0x2B]='}',
    [0x2C]='Z', [0x2D]='X', [0x2E]='C', [0x2F]='V', [0x30]='B',
    [0x31]='N', [0x32]='M', [0x33]='<', [0x34]='>', [0x35]=':',
    [0x37]='*', [0x39]=' ', [0x47]='7', [0x48]='8', [0x49]='9',
    [0x4A]='-', [0x4B]='4', [0x4C]='5', [0x4D]='6', [0x4E]='+',
    [0x4F]='1', [0x50]='2', [0x51]='3', [0x52]='0', [0x53]='.',
    [0x56]='>'
);

/* US: teclas que diferem do mapa brasileiro são ajustadas nas tabelas. */
KBD_MAP(kbd_us_lower,
    [0x01]=27, [0x02]='1', [0x03]='2', [0x04]='3', [0x05]='4',
    [0x06]='5', [0x07]='6', [0x08]='7', [0x09]='8', [0x0A]='9',
    [0x0B]='0', [0x0C]='-', [0x0D]='=', [0x0E]=8, [0x0F]='\t',
    [0x10]='q', [0x11]='w', [0x12]='e', [0x13]='r', [0x14]='t',
    [0x15]='y', [0x16]='u', [0x17]='i', [0x18]='o', [0x19]='p',
    [0x1A]='[', [0x1B]=']', [0x1C]='\n',
    [0x1E]='a', [0x1F]='s', [0x20]='d', [0x21]='f', [0x22]='g',
    [0x23]='h', [0x24]='j', [0x25]='k', [0x26]='l',
    [0x27]=';', [0x28]='\'', [0x29]='`', [0x2B]='\\',
    [0x2C]='z', [0x2D]='x', [0x2E]='c', [0x2F]='v', [0x30]='b',
    [0x31]='n', [0x32]='m', [0x33]=',', [0x34]='.', [0x35]='/',
    [0x37]='*', [0x39]=' ', [0x47]='7', [0x48]='8', [0x49]='9',
    [0x4A]='-', [0x4B]='4', [0x4C]='5', [0x4D]='6', [0x4E]='+',
    [0x4F]='1', [0x50]='2', [0x51]='3', [0x52]='0', [0x53]='.',
    [0x56]='\\'
);
KBD_MAP(kbd_us_upper,
    [0x01]=27, [0x02]='!', [0x03]='@', [0x04]='#', [0x05]='$',
    [0x06]='%', [0x07]='^', [0x08]='&', [0x09]='*', [0x0A]='(',
    [0x0B]=')', [0x0C]='_', [0x0D]='+', [0x0E]=8, [0x0F]='\t',
    [0x10]='Q', [0x11]='W', [0x12]='E', [0x13]='R', [0x14]='T',
    [0x15]='Y', [0x16]='U', [0x17]='I', [0x18]='O', [0x19]='P',
    [0x1A]='{', [0x1B]='}', [0x1C]='\n',
    [0x1E]='A', [0x1F]='S', [0x20]='D', [0x21]='F', [0x22]='G',
    [0x23]='H', [0x24]='J', [0x25]='K', [0x26]='L',
    [0x27]=':', [0x28]='"', [0x29]='~', [0x2B]='|',
    [0x2C]='Z', [0x2D]='X', [0x2E]='C', [0x2F]='V', [0x30]='B',
    [0x31]='N', [0x32]='M', [0x33]='<', [0x34]='>', [0x35]='?',
    [0x37]='*', [0x39]=' ', [0x47]='7', [0x48]='8', [0x49]='9',
    [0x4A]='-', [0x4B]='4', [0x4C]='5', [0x4D]='6', [0x4E]='+',
    [0x4F]='1', [0x50]='2', [0x51]='3', [0x52]='0', [0x53]='.',
    [0x56]='|'
);

/* ABNT mantém o mapa brasileiro básico; ABNT2 inclui a tecla extra < >. */
#define kbd_abnt_lower kbd_abnt2_lower
#define kbd_abnt_upper kbd_abnt2_upper

static unsigned char pending_dead_key = 0;

static unsigned char compose_dead_key(unsigned char dead, unsigned char c) {
    /* Composição Latin-1: acento morto + vogal. */
    switch (dead) {
        case DEAD_ACUTE:
            switch (c) {
                case 'a': return 160; case 'e': return 130;
                case 'i': return 161; case 'o': return 162;
                case 'u': return 163;
            } break;
        case DEAD_TILDE:
			if(c == 'n')return 164;
			if(c == 'N')return 165;
            if(c != 'N' && c == 'n')return 126;
        case DEAD_CIRCUMFLEX:
            switch (c) {
                case 'a': return 131; case 'e': return 136;
                case 'i': return 140; case 'o': return 147;
                case 'u': return 150; default: return 94;
            } break;
        case DEAD_GRAVE:
            switch (c) {
                case 'a': return 133; case 'e': return 138;
                case 'i': return 141; case 'o': return 149;
                case 'u': return 151; default: return 96;
            } break;
        case DEAD_DIAERESIS:
            switch (c) {
                case 'a': return 132; case 'e': return 137;
                case 'i': return 139; case 'o': return 148;
                case 'u': return 129;
            } break;
    }
    return 0;
}

static unsigned char scancode_ascii(unsigned char sc, int extended) {
    const unsigned char *lower;
    const unsigned char *upper;
    unsigned char normal, shifted;
    int shift = keyboard_info.mod.SHIFT_ON;
    int caps = keyboard_info.mod.CAPS_ON;

    if (extended) {
        if (sc == 0x1C) return '\n';
        if (sc == 0x35) return '/';
        return 0;
    }
    if (sc >= 128) return 0;

    if (ps2_layout == US) {
        lower = kbd_us_lower;
        upper = kbd_us_upper;
    } else if (ps2_layout == ABNT) {
        lower = kbd_abnt_lower;
        upper = kbd_abnt_upper;
    } else {
        lower = kbd_abnt2_lower;
        upper = kbd_abnt2_upper;
    }

    normal = lower[sc];
    shifted = upper[sc];
    if (normal >= 'a' && normal <= 'z')
        normal = (shift ^ caps) ? shifted : normal;
    else
        normal = shift ? shifted : normal;

    if (normal >= DEAD_ACUTE && normal <= DEAD_GRAVE) {
        pending_dead_key = normal;
        return 0;
    }

    /*
     * Tecla morta: ela própria nunca gera ASCII. Mantemos o acento pendente
     * até chegar um caractere imprimível. Assim, Shift/Ctrl/Alt e outras
     * teclas sem ASCII não consomem o acento.
     * Se a próxima tecla imprimível não aceitar aquele acento, descartamos
     * somente o acento pendente e entregamos o caractere original.
     */
    if (pending_dead_key && normal != 0) {
        unsigned char composed = compose_dead_key(pending_dead_key, normal);
        pending_dead_key = 0;
        if (composed) return composed;
    }
    return normal;
}

static void keyboard_byte(unsigned char value) {
    unsigned char sc;
    int release, extended;
    unsigned short raw;

    /* Pause/Break usa uma sequência E1 de vários bytes; ignoramos o evento. */
    if (pause_bytes) {
        --pause_bytes;
        return;
    }
    if (value == 0xE1) {
        pause_bytes = 5;
        e0_prefix = 0;
        return;
    }
    if (value == 0xE0) {
        e0_prefix = 1;
        return;
    }

    extended = e0_prefix;
    e0_prefix = 0;
    release = (value & 0x80) != 0;
    sc = value & 0x7F;
    raw = (unsigned short)(sc | (extended ? 0xE000 : 0));

    if (!extended && (sc == 0x3A || sc == 0x45 || sc == 0x46) && !release) {
        if (sc == 0x3A) caps_lock ^= 1;
        if (sc == 0x45) num_lock ^= 1;
        if (sc == 0x46) scroll_lock ^= 1;
    }

    if (extended) {
        if (sc == 0x1D) key_down[0x9D] = !release; /* right Ctrl */
        else if (sc == 0x38) key_down[0xB8] = !release; /* right Alt */
		else key_down[sc] = !release; /* tira bug da cetinha (atualização) */
    } else {
        key_down[sc] = !release;
    }

    update_modifiers();
    keyboard_info.ps2key = raw;
    keyboard_info.keyDown = release ? 0 : 1;
    keyboard_info.keyPress = release ? 0 : 1;
    keyboard_info.asciiKey = release ? 0 : scancode_ascii(sc, extended);
}

static void mouse_byte(unsigned char value) {
    int dx, dy, wheel = 0;
    unsigned char flags;

    if (mouse_packet_pos == 0 && !(value & 0x08)) return; /* alinhamento */
    mouse_packet[mouse_packet_pos++] = value;
    if (mouse_packet_pos < mouse_packet_size) return;
    mouse_packet_pos = 0;

    flags = mouse_packet[0];
    dx = (int)mouse_packet[1];
    dy = (int)mouse_packet[2];
    if (flags & 0x10) dx -= 256;
    if (flags & 0x20) dy -= 256;

    mouse_info.x += dx;
    mouse_info.y -= dy; /* coordenadas de tela: para baixo é positivo */
    mouse_info.left   = flags & 0x01;
    mouse_info.right  = (flags >> 1) & 1;
    mouse_info.middle = (flags >> 2) & 1;

    if (mouse_packet_size == 4) {
        wheel = mouse_packet[3] & 0x0F;
        if (wheel & 0x08) wheel -= 16;
        mouse_info.scroll += wheel;
    }
    mouse_info.packetReady = 1;
}

int InitPS2(void) {
    unsigned char config, result, id;
    unsigned int i;

    ps2_initialized = 0;
    keyboard_info.keyPress = 0;
    keyboard_info.asciiKey = 0;
    keyboard_info.ps2key = 0;
    mouse_info.x = mouse_info.y = mouse_info.scroll = 0;
    mouse_info.packetReady = 0;
    mouse_packet_pos = 0;
    mouse_packet_size = 3;
    mouse_id = 0;
    e0_prefix = pause_bytes = 0;
    pending_dead_key = 0;
    for (i = 0; i < 256; ++i) key_down[i] = 0;
    caps_lock = num_lock = scroll_lock = 0;
    update_modifiers();

    /* Desabilita os dois canais e esvazia qualquer dado antigo. */
    if (!controller_command(0xAD) || !controller_command(0xA7)) return 0;
    for (i = 0; i < 32 && (tinb(PS2_STATUS) & PS2_STATUS_OBF); ++i)
        (void)tinb(PS2_DATA);

    /* Lê configuração; polling não usa IRQ1/IRQ12. Habilita tradução Set 1. */
    if (!controller_command(0x20) || !controller_read_data(&config)) return 0;
    config &= (unsigned char)~0x03; /* IRQs desligadas */
    config |= 0x40;                 /* tradução teclado para Set 1 */
    config &= (unsigned char)~0x30; /* libera clocks dos dois canais */
    if (!controller_command(0x60) || !controller_write_data(config)) return 0;

    /* Self-test do controlador. Alguns controladores resetam a config, então regrava. */
    if (!controller_command(0xAA) || !controller_read_data(&result) || result != 0x55)
        return 0;
    if (!controller_command(0x60) || !controller_write_data(config)) return 0;

    if (!controller_command(0xAE) || !controller_command(0xA8)) return 0;

    /* Mouse: defaults, tenta habilitar roda (ID 03), depois streaming. */
    if (!mouse_write(0xF6)) return 0;
    (void)mouse_write(0xF3); (void)mouse_write(200);
    (void)mouse_write(0xF3); (void)mouse_write(100);
    (void)mouse_write(0xF3); (void)mouse_write(80);
    if (mouse_write(0xF2) && controller_read_data(&id)) {
        mouse_id = id;
        if (id == 0x03 || id == 0x04) mouse_packet_size = 4;
    }
    if (!mouse_write(0xF4)) return 0;

    /* Habilita o scanning do teclado por último, após configurar o mouse. */
    if (!keyboard_write(0xF4)) return 0;

    ps2_initialized = 1;
    return 1;
}

void ReadPS2(void) {
    unsigned int count = 0;
    if (!ps2_initialized) return;

    /* Drena os dois canais; o bit AUX identifica bytes vindos do mouse. */
    while ((tinb(PS2_STATUS) & PS2_STATUS_OBF) && count++ < 64) {
        unsigned char status = tinb(PS2_STATUS);
        unsigned char value = tinb(PS2_DATA);
        if (status & (PS2_STATUS_TIMEOUT | PS2_STATUS_PARITY)) continue;
        if (status & PS2_STATUS_AUX) mouse_byte(value);
        else {
            keyboard_byte(value);
            /* Um byte de teclado por chamada: não sobrescreve um segundo evento
               antes que o kernel tenha chance de chamar GetKeyboardInfo(). */
            break;
        }
    }
}

void GetMouseInfo(PS2MouseInfo *mouse) {
    if (!mouse) return;
    *mouse = mouse_info;
    mouse_info.x = 0;
    mouse_info.y = 0;
    mouse_info.scroll = 0;
    mouse_info.packetReady = 0;
}

void GetKeyboardInfo(PS2KeyboardInfo *kbd) {
    if (!kbd) return;
    *kbd = keyboard_info;
    keyboard_info.keyPress = 0;
    keyboard_info.asciiKey = 0;
}

void LayoutKbdPS2(PS2KeyboardLayout layout) {
    if (layout == ABNT2 || layout == US || layout == ABNT)
        ps2_layout = layout;
}

void GetPS2Modifiers(PS2Modifiers *mods) {
    if (mods) *mods = keyboard_info.mod;
}

int PS2IsInitialized(void) {
    return ps2_initialized;
}
