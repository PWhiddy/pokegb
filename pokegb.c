#include <SDL2/SDL.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

/* --- Forward Declarations (Required for C) --- */
void tick();
uint8_t mem8(uint16_t addr, uint8_t val, int write);
uint8_t reg8_access(uint8_t val, int write, uint8_t o);
uint16_t read16(uint16_t *addr);
void push(uint16_t val);
void set_flags(uint8_t mask, int Z, int N, int H, int C);

/* --- Macros --- */
#define OPCREL(_) opcrel = (opcode - _) / 8

#define OP4_NX8(_,X) case _: case _ + 8*X: case _ + 16*X: case _ + 24*X:

#define OP4_NX16_REL(_) OP4_NX8(_, 2) opcrel = (opcode - _) / 16;

#define OP5_FLAG(_, always)                                                    \
  OP4_NX8(_, 1)                                                                \
  case always:                                                                 \
    OPCREL(_), carry = opcode == always || !(*F & F_mask[opcrel]) ^ (opcrel & 1);

#define OP8_REL(_)                                                             \
  case _ ... _ + 7:                                                            \
    tmp8 = reg8_access(0, 0, opcrel = opcode);

#define OP8_NX8_REL(_)                                                         \
  OP4_NX8(_, 1) OP4_NX8(_ + 32, 1) tmp8 = reg8_access(0, 0, OPCREL(_));

#define OP64_REL(_)                                                            \
  case _ ... _ + 55: OP8_REL(_ + 56) OPCREL(_);

#define OP9_IMM_PTR(_)                                                         \
  OP8_REL(_) case _ + 70 : operand = opcode & 64 ? mem8(PC++, 0, 0) : tmp8;

/* --- Global State --- */
uint8_t opcode, opcrel, tmp8, operand, carry, neg, *rom0, *rom1, io[512],
    video_ram[8192], work_ram[16384], *extram, *extrambank,
    reg8[] = {19, 0, 216, 0, 77, 1, 176, 1}, 
    *F, *A, *reg8_group[8], 
    *IF, *LCDC, *LY, IME, halt;

uint8_t const *key_state;

uint16_t PC = 256, *reg16, *HL, SP = 65534,
         *DIV, ppu_dot = 32,
         *reg16_group1[4], *reg16_group2[4], prev_cycles, cycles;

int tmp, tmp2, F_mask[] = {128, 128, 16, 16}, frame_buffer[23040],
               palette[] = {-1, -23197,   -65536,    -1 << 24,
                            -1, -8092417, -12961132, -1 << 24};

/* --- Implementation --- */

void tick() { cycles += 4; }

uint8_t mem8(uint16_t addr, uint8_t val, int write) {
  tick();
  switch (addr >> 13) {
    case 1: if (write) rom1 = rom0 + ((val ? val & 63 : 1) << 14);
            // fallthrough
    case 0: return rom0[addr];
    case 2: if (write && val <= 3) extrambank = extram + (val << 13);
            // fallthrough
    case 3: return rom1[addr & 16383];
    case 4: addr &= 8191; if (write) video_ram[addr] = val; return video_ram[addr];
    case 5: addr &= 8191; if (write) extrambank[addr] = val; return extrambank[addr];
    case 7:
      if (addr >= 65024) {
        if (write) {
          // OAM DMA Transfer
          if (addr == 65350) for (int y = 160; --y >= 0;) io[y] = mem8((val << 8 | y), 0, 0);
          io[addr & 511] = val;
        }

        if (addr == 65280) { // Joypad
          if (~io[256] & 16) 
            return ~(16 + key_state[SDL_SCANCODE_DOWN] * 8 + key_state[SDL_SCANCODE_UP] * 4 + key_state[SDL_SCANCODE_LEFT] * 2 + key_state[SDL_SCANCODE_RIGHT]);
          if (~io[256] & 32) 
            return ~(32 + key_state[SDL_SCANCODE_RETURN] * 8 + key_state[SDL_SCANCODE_TAB] * 4 + key_state[SDL_SCANCODE_Z] * 2 + key_state[SDL_SCANCODE_X]);
          return 255;
        }
        return io[addr & 511];
      }
      // fallthrough
    case 6: addr &= 16383; if (write) work_ram[addr] = val; return work_ram[addr];
  }
  return 0;
}

void set_flags(uint8_t mask, int Z, int N, int H, int C) {
  *F = (*F & mask) | (!Z << 7) | (N << 6) | (H << 5) | (C << 4);
}

uint16_t read16(uint16_t *addr) {
  tmp8 = mem8((*addr)++, 0, 0);
  return mem8((*addr)++, 0, 0) << 8 | tmp8;
}

void push(uint16_t val) {
  mem8(--SP, val >> 8, 1);
  mem8(--SP, val, 1);
  tick();
}

uint8_t reg8_access(uint8_t val, int write, uint8_t o) {
  return (o &= 7) == 6 ? mem8(*HL, val, write)
          : write       ? (*reg8_group[o] = val)
                        : *reg8_group[o];
}

uint8_t get_color(int tile, int y_offset, int x_offset) {
  uint8_t *tile_data = &video_ram[tile * 16 + y_offset * 2];
  return (tile_data[1] >> x_offset) % 2 * 2 + (*tile_data >> x_offset) % 2;
}

int main() {
  /* Pointer Initialization (Replaces C++ References) */
  reg16 = (uint16_t *)reg8; 
  HL = &reg16[2];
  F = &reg8[6]; 
  A = &reg8[7];
  
  // Register mapping: B, C, D, E, H, L, F, A
  reg8_group[0] = &reg8[1]; reg8_group[1] = &reg8[0];
  reg8_group[2] = &reg8[3]; reg8_group[3] = &reg8[2];
  reg8_group[4] = &reg8[5]; reg8_group[5] = &reg8[4];
  reg8_group[6] = F;         reg8_group[7] = A;
  
  reg16_group1[0] = reg16; reg16_group1[1] = reg16 + 1; reg16_group1[2] = HL; reg16_group1[3] = &SP;
  reg16_group2[0] = reg16; reg16_group2[1] = reg16 + 1; reg16_group2[2] = HL; reg16_group2[3] = HL;

  IF = &io[271]; LCDC = &io[320]; LY = &io[324]; DIV = (uint16_t *)&io[259];

  /* File Loading & Memory Mapping */
  int rom_fd = open("rom.gb", O_RDONLY);
  if (rom_fd < 0) return 1; // Basic error handling
  
  rom0 = (uint8_t *)mmap(0, 1 << 20, PROT_READ, MAP_SHARED, rom_fd, 0);
  rom1 = rom0 + 32768;

  tmp = open("rom.sav", O_CREAT|O_RDWR, 0666);
  ftruncate(tmp, 32768);
  extrambank = extram = (uint8_t *)mmap(0, 32768, PROT_READ | PROT_WRITE, MAP_SHARED, tmp, 0);

  *LCDC = 145; 
  *DIV = 44032;

  SDL_Init(SDL_INIT_VIDEO);
  SDL_Renderer *renderer = SDL_CreateRenderer(SDL_CreateWindow("pokegb", 0, 0, 800, 720, SDL_WINDOW_SHOWN), -1, SDL_RENDERER_PRESENTVSYNC);
  SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, 160, 144);
  key_state = SDL_GetKeyboardState(0);

  /* Main Emulation Loop */
  while (1) {
    prev_cycles = cycles;
    if (IME && (*IF & io[511])) {
      *IF = halt = IME = 0;
      cycles += 8;
      push(PC);
      PC = 64;
    } else if (halt) {
      tick();
    } else {
      switch (opcode = mem8(PC++, 0, 0)) {
        OP4_NX16_REL(1) *reg16_group1[opcrel] = read16(&PC); break;
        case 0: break;
        OP4_NX16_REL(10) OP4_NX16_REL(2)
          tmp = opcode & 8;
          reg8_access(mem8(*reg16_group2[opcrel], *A, !tmp), tmp, 7);
          *HL += opcrel < 2 ? 0 : 5 - 2 * opcrel;
          break;
        OP4_NX16_REL(11) OP4_NX16_REL(3) *reg16_group1[opcrel] += opcode & 8 ? -1 : 1; tick(); break;
        OP8_NX8_REL(5) OP8_NX8_REL(4) neg = opcode & 1; reg8_access(tmp8 += 1 - neg * 2, 1, opcrel); set_flags(16, tmp8, neg, !(tmp8 + neg & 15), 0); break;
        OP8_NX8_REL(6) reg8_access(mem8(PC++, 0, 0), 1, opcrel); break;
        OP4_NX16_REL(9) tmp = *reg16_group1[opcrel]; set_flags(128, 1, 0, *HL % 4096 + tmp % 4096 > 4095, *HL + tmp > 65535); *HL += tmp; tick(); break;
        OP4_NX8(7,1) neg = 1; goto ROTATE;
        OP5_FLAG(32, 24) tmp8 = mem8(PC++, 0, 0); if (carry) PC += (int8_t)tmp8, tick(); break;
        case 39: carry = tmp8 = 0; if (*F & 32 || (~*F & 64 && *A % 16 > 9)) tmp8 = 6; if (*F & 16 || (~*F & 64 && *A > 153)) tmp8 |= 96, carry = 1; set_flags(65, *A += *F & 64 ? -tmp8 : tmp8, 0, 0, carry); break;
        case 47: *A = ~*A; set_flags(144, 1, 1, 1, 0); break;
        case 55: case 63: set_flags(128, 1, 0, 0, opcode & 8 ? !(*F & 16) : 1); break;
        OP64_REL(64) opcode == 118 ? (halt = 1) : reg8_access(tmp8, 1, opcrel); break;
        OP9_IMM_PTR(128) neg = carry = 0; goto ALU;
        OP9_IMM_PTR(136) neg = 0; carry = *F / 16 % 2; goto ALU;
        OP9_IMM_PTR(184) goto SUB;
        OP9_IMM_PTR(144) SUB: carry = 1; goto SUBTRACT;
        OP9_IMM_PTR(152) carry = !(*F / 16 % 2);
        SUBTRACT: neg = 1; operand = ~operand;
        ALU: set_flags(0, tmp8 = *A + operand + carry, neg, (*A % 16 + operand % 16 + carry > 15) ^ neg, (*A + operand + carry > 255) ^ neg); if (~(opcode / 8) & 7) *A = tmp8; break;
        OP9_IMM_PTR(160) set_flags(0, *A &= operand, 0, 1, 0); break;
        OP9_IMM_PTR(168) set_flags(0, *A ^= operand, 0, 0, 0); break;
        OP9_IMM_PTR(176) set_flags(0, *A |= operand, 0, 0, 0); break;
        case 217: carry = IME = 1; goto RET;
        OP5_FLAG(192, 201) RET: tick(); if (carry) PC = read16(&SP); break;
        OP4_NX16_REL(193) reg16[opcrel] = read16(&SP); break;
        OP5_FLAG(194, 195) goto CALL;
        OP5_FLAG(196, 205) CALL: tmp = read16(&PC); if (carry) opcode & 4 ? push(PC) : tick(), PC = tmp; break;
        OP4_NX16_REL(197) push(reg16[opcrel]); break;
        case 203: neg = 0; opcode = mem8(PC++, 0, 0);
          ROTATE: switch (opcode) {
            OP8_REL(0) OP8_REL(16) OP8_REL(32) carry = tmp8 >> 7; tmp8 += tmp8 + (opcode & 16 ? *F / 16 % 2 : opcode & 32 ? 0 : carry); goto CARRY_ZERO_FLAGS_U;
            OP8_REL(48) carry = 0; tmp8 = tmp8 * 16 + tmp8 / 16; goto CARRY_ZERO_FLAGS_U;
            OP8_REL(8) OP8_REL(24) OP8_REL(40) OP8_REL(56) carry = tmp8 & 1; tmp8 = (opcode & 48) == 32 ? (int8_t)tmp8 >> 1 : tmp8 / 2 + (opcode & 32 ? 0 : opcode & 16 ? (*F * 8 & 128) : carry * 128);
            CARRY_ZERO_FLAGS_U: reg8_access(tmp8, 1, opcrel); set_flags(0, neg || tmp8, 0, 0, carry); break;
            OP64_REL(64) set_flags(16, tmp8 & 1 << opcrel, 0, 1, 0); break;
            OP64_REL(128) reg8_access(tmp8 & ~(1 << opcrel), 1, opcode); break;
            OP64_REL(192) reg8_access(tmp8 | 1 << opcrel, 1, opcode); break;
          } break;
        case 224: case 226: case 234: case 240: case 242: case 250:
          tmp = opcode & 16;
          // IMPORTANT: Nested call requires explicit arguments in C
          reg8_access(mem8(opcode & 8 ? read16(&PC) : 65280 + (opcode & 2 ? *reg8 : mem8(PC++, 0, 0)), *A, !tmp), tmp, 7); 
          break;
        case 233: PC = *HL; break;
        case 243: case 251: IME = opcode != 243; break;
        case 248: *HL = SP + (int8_t)(tmp8 = mem8(PC++, 0, 0)); set_flags(0, 1, 0, SP % 16 + tmp8 % 16 > 15, (uint8_t)SP + tmp8 > 255); tick(); break;
        case 249: SP = *HL; tick(); break;
      }
    }

    /* PPU (Video) Logic */
    for (*DIV += cycles - prev_cycles; prev_cycles++ != cycles;)
      if (*LCDC & 128) {
        if (++ppu_dot == 456) {
          if (*LY < 144)
            for (tmp = 160; --tmp >= 0;) {
              uint8_t is_window = *LCDC & 32 && *LY >= io[330] && tmp >= io[331] - 7,
                      x_offset = is_window ? tmp - io[331] + 7 : tmp + io[323],
                      y_offset = is_window ? *LY - io[330] : *LY + io[322];
              uint16_t palette_index = 0,
                       tile = video_ram[(*LCDC & (is_window ? 64 : 8) ? 7 : 6) << 10 | y_offset / 8 * 32 + x_offset / 8],
                       color = get_color(*LCDC & 16 ? tile : 256 + (int8_t)tile, y_offset & 7, 7 - x_offset & 7);
              if (*LCDC & 2)
                for (uint8_t *sprite = io; sprite < io + 160; sprite += 4) {
                  uint8_t sprite_x = tmp - sprite[1] + 8, sprite_y = *LY - *sprite + 16,
                          sprite_color = get_color(sprite[2], sprite_y ^ (sprite[3] & 64 ? 7 : 0), sprite_x ^ (sprite[3] & 32 ? 0 : 7));
                  if (sprite_x < 8 && sprite_y < 8 && !(sprite[3] & 128 && color) && sprite_color) {
                    color = sprite_color; palette_index = 1 + !!(sprite[3] & 16); break;
                  }
                }
              frame_buffer[*LY * 160 + tmp] = palette[(io[327 + palette_index] >> 2 * color) % 4 + palette_index * 4 & 7];
            }
          if (*LY == 143) {
            *IF |= 1;
            SDL_UpdateTexture(texture, 0, frame_buffer, 640);
            SDL_RenderCopy(renderer, texture, 0, 0);
            SDL_RenderPresent(renderer);
            SDL_Event event;
            while (SDL_PollEvent(&event)) if (event.type == SDL_QUIT) return 0;
          }
          *LY = (*LY + 1) % 154; ppu_dot = 0;
        }
      } else *LY = ppu_dot = 0;
  }
}
