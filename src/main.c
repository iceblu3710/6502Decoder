#include <stdio.h>
#include <inttypes.h>

#include "em_6502.h"

enum Cycle {
   Cycle_FETCH,
   Cycle_OP1,
   Cycle_OP2,
   Cycle_MEMRD,
   Cycle_MEMWR
};


unsigned char * cycle_to_name_map[] = {
   "Fetch",
   "Op1",
   "Op2",
   "Read",
   "Write",
};

#define BUFSIZE 8192

uint16_t buffer[BUFSIZE];

void decode(FILE *stream) {

   InstrType *instr         = NULL;

   int cyclenum             = 0;
   int last_sync_cyclenum   = 0;
   int opcount              = 0;
   int cycle                = Cycle_MEMRD;
   int opcode               = -1;
   int op1                  = 0;
   int op2                  = 0;
   int operand              = 0;
   int write_count          = 0;
   int pc                   = -1;
   int read_accumulator     = 0;
   int write_accumulator    = 0;
   int do_emulate           = 1;


   unsigned char target[16];

   int newpc = 0;
   int num                  = 0;

   // TODO: make these configurable
   int idx_data = 0;
   int idx_rnw = 8;
   int idx_sync = 9;
   int idx_rdy = 10;
   int idx_phi2 = 11;

   int bus_data = 0;
   int pin_rnw = 0;
   int pin_sync = 0;
   int pin_rdy = 0;
   int pin_phi2 = 0;

   int last_phi2 = -1;

   while ((num = fread(buffer, sizeof(uint16_t), BUFSIZE, stream)) > 0) {

      uint16_t *sampleptr = &buffer[0];

      while (num-- > 0) {

         uint16_t sample = *sampleptr++;


         // Phi2 is optional
         // - if asynchronous capture is used, it must be connected
         // - if synchronous capture is used, it must not connected
         if (idx_phi2 < 0) {

            // If Phi2 is not present, use the pins directly
            bus_data  = (sample >> idx_data) & 255;
            pin_rnw   = (sample >> idx_rnw) & 1;
            pin_sync  = (sample >> idx_sync) & 1;
            pin_rdy   = (sample >> idx_rdy) & 1;

         } else {

            // TODO: make this faster by using last_sample

            // If Phi2 is present, look for the falling edge, and proceed with the previous sample
            pin_phi2 = (sample >> idx_phi2) & 1;
            if (pin_phi2 == 1 || last_phi2 != 1) {
               last_phi2 = pin_phi2;
               bus_data  = (sample >> idx_data) & 255;
               pin_rnw   = (sample >> idx_rnw) & 1;
               pin_sync  = (sample >> idx_sync) & 1;
               pin_rdy   = (sample >> idx_rdy) & 1;
               continue;
            }
         }

         // Resample read data just after the falling edge, as there should be reasonable hold time
         if (pin_rnw == 1) {
            bus_data  = (sample >> idx_data) & 255;
         }

         // At this point, either phi2 is not connected, or last_phi2 = 1 and phi2 = 0 (i.e. falling edge)
         last_phi2 = 0;

         // Ignore the cycle if RDY is low
         if (pin_rdy == 0)
            continue;

         // Sync indicates the start of a new instruction, the following variables pertain to the previous instruction
         // opcode, op1, op2, read_accumulator, write_accumulator, write_count

         if (pin_sync == 1) {
            int numchars;

            if (opcode >= 0) {
               instr    = &instr_table[opcode];


               // For instructions that push the current address to the stack we
               // can use the stacked address to determine the current PC
               newpc = -1;
               if (write_count == 3) {
                  // IRQ/NMI/RST
                  newpc = (write_accumulator >> 8) & 0xffff;
               } else if (opcode == 0x20) {
                  // JSR
                  newpc = (write_accumulator - 2) & 0xffff;
               }

               // Sanity check the current pc prediction has not gone awry
               if (newpc >= 0) {
                  if (pc >= 0 && pc != newpc) {
                     printf("pc: prediction failed at %04X old pc was %04X\n", newpc, pc);
                     pc = newpc;
                  }
               }

               if (pc < 0) {
                  printf(" ????: ");
               } else {
                  printf(" %04X: ", pc);
               }
               if (write_count == 3 && opcode != 0) {
                  // Annotate an interrupt
                  numchars = printf("INTERRUPT !!");
                  if (do_emulate) {
                     em_interrupt(write_accumulator & 0xff);
                  }
               } else {
                  // Calculate branch target using op1 for normal branches and op2 for BBR/BBS
                  int offset = (char) ((opcode & 0x0f == 0x0f)  ? op2 : op1);
                  if (pc < 0) {
                     if (offset < 0) {
                        sprintf(target, "pc-%d", -offset);
                     } else {
                        sprintf(target,"pc-%d", offset);
                     }
                  } else {
                     sprintf(target, "%04X", pc + 2 + offset);
                  }

                  // Annotate a normal instruction
                  int mode = instr->mode;
                  const char *mnemonic = instr->mnemonic;
                  const char *fmt = instr->fmt;
                  if (mode <= IMPA) {
                     numchars = printf(fmt, mnemonic);
                  } else if (mode == BRA) {
                     numchars = printf(fmt, mnemonic, target);
                  } else if (mode <= IND) {
                     numchars = printf(fmt, mnemonic, op1);
                  } else if (mode <= IND1X) {
                     numchars = printf(fmt, mnemonic, op1, op2);
                  } else {
                     numchars = printf(fmt, mnemonic, op1, target);
                  }

                  // Emulate the instruction
                  if (do_emulate) {
                     if (instr->emulate) {
                        if (opcode == 0x40) {
                           // special case RTI, operand (flags) is the first read cycle of three
                           operand = read_accumulator & 0xff;
                        }
                        if (instr->optype == WRITEOP) {
                           // special case instructions where the operand is being written (STA/STX/STY/PHP/PHA/PHX/PHY/BRK)
                           operand = write_accumulator & 0xff;
                        } else if (instr->optype == BRANCHOP) {
                           // special case branch instructions, operand is true if branch taken
                           operand = (cyclenum - last_sync_cyclenum != 2);
                        }
                        instr->emulate(operand);
                     }
                  }
               }

               if (do_emulate) {
                  // Pad opcode to 20 characters
                  while (numchars++ < 14) {
                     printf(" ");
                  }
                  printf("%s\n", em_get_state());
               } else {
                  printf("\n");
               }

               // Look for control flow changes and update the PC
               if (opcode == 0x40 || opcode == 0x00 || opcode == 0x6c || opcode == 0x7c || write_count == 3) {
                  // RTI, BRK, INTR, JMP (ind), JMP (ind, X), IRQ/NMI/RST
                  pc = (read_accumulator >> 8) & 0xffff;
               } else if (opcode == 0x20 || opcode == 0x4c) {
                  // JSR abs, JMP abs
                  pc = op2 << 8 | op1;
               } else if (opcode == 0x60) {
                  // RTS
                  pc = (read_accumulator + 1) & 0xffff;
               } else if (pc < 0) {
                  // PC value is not known yet, everything below this point is relative
                  pc = -1;
               } else if (opcode == 0x80) {
                  // BRA
                  pc += ((char)(op1)) + 2;
               } else if ((opcode & 0x0f) == 0x0f && cyclenum - last_sync_cyclenum != 2) {
                  // BBR/BBS
                  pc += ((char)(op2)) + 2;
               } else if ((opcode & 0x1f) == 0x10 && cyclenum - last_sync_cyclenum != 2) {
                  // BXX: op1 if taken
                  pc += ((char)(op1)) + 2;
               } else {
                  // Otherwise, increment pc by length of instuction
                  pc += instr->len;
               }
            }

            last_sync_cyclenum  = cyclenum;

            // Start decoding a new instruction
            cycle             = Cycle_FETCH;
            opcode            = bus_data;
            opcount           = instr_table[opcode].len - 1;
            write_count       = 0;
            operand           = -1;
            read_accumulator  = 0;
            write_accumulator = 0;

         } else if (pin_rnw == 0) {
            cycle = Cycle_MEMWR;
            write_count += 1;
            write_accumulator = (write_accumulator << 8) | bus_data;

         } else if (cycle == Cycle_FETCH && opcount > 0) {
            cycle = Cycle_OP1;
            opcount -= 1;
            op1 = bus_data;
            operand = bus_data;

         } else if (cycle == Cycle_OP1 && opcount > 0) {
            if (opcode == 0x20) { // JSR is <opcode> <op1> <dummp stack rd> <stack wr> <stack wr> <op2>
               cycle = Cycle_MEMRD;
            } else {
               cycle = Cycle_OP2;
               opcount -= 1;
               op2 = bus_data;
            }

         } else {
            if (opcode == 0x20){ // JSR, see above
               cycle = Cycle_OP2;
               opcount -= 1;
               op2 = bus_data;
            } else {
               cycle = Cycle_MEMRD;
               operand = bus_data;
               read_accumulator = (read_accumulator >> 8) | (bus_data << 16);
            }
         }


         // Increment the cycle number (used only to detect taken branches)
         cyclenum += 1;

      }
   }
}

int main(int argc, char *argv[]) {
   em_init();
   if (argc != 2) {
      fprintf(stderr, "usage: %s <capture file>\n", argv[0]);
      return 1;
   }
   FILE *stream = fopen(argv[1], "r");
   if (stream == NULL) {
      perror("failed to open capture file");
      return 2;
   }
   decode(stream);
   fclose(stream);
   return 0;
}
