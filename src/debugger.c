#include <stdio.h>
#include <string.h>

#include "core_interface.h"
#include "debugger.h"

// For usleep
#include <unistd.h>

/*
 * Variables
 */

// General Purpose Register names
const char *register_names[] = {
    "$r0",
    "$at",
    "v0", "v1",
    "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9",
    "k0", "k1",
    "$gp",
    "$sp",
    "sB",
    "$ra"
};

// Holds the previous GPR values for comparison details.
long long int prev_reg_values[32];
char reg_ran_previously = 0;

// Used to wait for core response before requesting next command.
int debugger_loop_wait = 1;

// Counter indicating the number of DebugStep() calls we need to make yet.
static int debugger_steps_pending = 0;

// Keep track of the run state.
static int cur_run_state = 0;

// Remember the current program counter.
static unsigned int cur_pc = 0;

// Keep track of breakpoints locally.
static m64p_breakpoint *breakpoints;
static int num_breakpoints = 0;

/*
 * Debugger callbacks.
 */
void dbg_frontend_init() {
    breakpoints = malloc(BREAKPOINTS_MAX_NUMBER * sizeof(m64p_breakpoint));
    printf("Debugger initialized.\n");
}

void dbg_frontend_update(unsigned int pc) {
    cur_pc = pc;
    if (!debugger_steps_pending) {
        printf("\nPC at 0x%08X.\n", pc);
        debugger_loop_wait = 0;
        cur_run_state = 0;
    }
    else {
        --debugger_steps_pending;
        debugger_step();
    }
}

void dbg_frontend_vi() {
    //printf("Debugger vertical int.\n");
}

/*
 * Debugger methods.
 */
int debugger_setup_callbacks() {
    m64p_error rval = (*DebugSetCallbacks)(dbg_frontend_init,
                                           dbg_frontend_update,
                                           dbg_frontend_vi);
    return rval != M64ERR_SUCCESS;
}

int debugger_set_run_state(int state) {
    m64p_error rval = (*DebugSetRunState)(state);
    return rval != M64ERR_SUCCESS;
}

int debugger_step() {
    m64p_error rval = (*DebugStep)();
    return rval != M64ERR_SUCCESS;
}

// Retrieve the program counter.
int debugger_get_prev_pc() {
    return (*DebugGetState)(M64P_DBG_PREVIOUS_PC);
}

int debugger_read_64(unsigned int addr) {
    return (*DebugMemRead64)(addr);
}
int debugger_read_32(unsigned int addr) {
    return (*DebugMemRead32)(addr);
}
int debugger_read_16(unsigned int addr) {
    return (*DebugMemRead16)(addr);
}
int debugger_read_8(unsigned int addr) {
    return (*DebugMemRead8)(addr);
}

int debugger_print_registers() {
    unsigned long long int *regs = (*DebugGetCPUDataPtr)(M64P_CPU_REG_REG);
    if (regs == NULL)
        return -1;

    printf("General Purpose Registers:\n");
    int i;
    const char *format_padded = "%4s %016X ";
    const char *format_nopad = "%4s %16X ";
    for (i = 0; i < 32; ++i) {
        char val_changed = reg_ran_previously && regs[i] != prev_reg_values[i];

        // Use bold font if the value has changed since last time.
        if (val_changed)
            printf("%c[1m", 27); // Bold on

        // Print the register value, no padding if it is all zeroes. 
        printf(regs[i] == 0 ? format_nopad : format_padded,
               register_names[i], regs[i]);

        // Unset bold.
        if (val_changed)
            printf("%c[0m", 27); // Bold off

        reg_ran_previously = 1;
        prev_reg_values[i] = regs[i];

        // Two registers per line.
        if (i % 2 != 0)
            printf("\n");
    }
    return 0;
}

char *debugger_decode_op(unsigned int instruction, int instruction_addr,
                         char *output) {
    if (output == NULL)
        output = calloc(40, sizeof(char));

    (*DebugDecodeOp)(instruction, output, output + 10, instruction_addr);
    return output;
}

/*
 * Debugger main loop
 */
int debugger_loop(void *arg) {
    char input[256];
    while (1) {
        if (debugger_loop_wait) {
            usleep(100);
            continue;
        }

        printf("(dbg) ");
        fgets(input, 256, stdin);
        input[strlen(input) - 1] = 0;

        if (strcmp(input, "run") == 0) {
            cur_run_state = 2;
            if (debugger_set_run_state(cur_run_state))
                printf("Error setting run_state: run\n");
            else {
                debugger_step(); // Hack to kick-start the emulation.
            }
        }
        else if (strcmp(input, "pause") == 0) {
            cur_run_state = 0;
            if (debugger_set_run_state(cur_run_state))
                printf("Error setting run_state: pause\n");
        }
        else if (strncmp(input, "step", 4) == 0) {
            if (cur_run_state == 2) {
              printf("Cannot step while running. Type `pause' first.\n");
              continue;
            }

            debugger_loop_wait = 1;
            debugger_steps_pending = 1;
            sscanf(input, "step %d", &debugger_steps_pending);
            if (debugger_steps_pending < 1)
                debugger_steps_pending = 1;
            --debugger_steps_pending;
            debugger_step();
        }
        else if (strcmp(input, "regs") == 0) {
            debugger_print_registers();
        }
        else if (strcmp(input, "pc") == 0) {
            printf("PC: %08X\n", cur_pc);
        }
        else if (strcmp(input, "pc-1") == 0) {
            printf("Previous PC: %08X\n", debugger_get_prev_pc());
        }
        else if (strcmp(input, "asm") == 0) {
            char *decoded = debugger_decode_op(debugger_read_32(cur_pc),
                                               cur_pc,
                                               NULL);
            printf(decoded);
            printf(" %s\n", decoded + 10);
        }
        else if (strcmp(input, "bp list") == 0 || strcmp(input, "bp ls") == 0) {
            if (num_breakpoints == 0) {
                printf("No breakpoints added. Add with 'bp add 0x...'\n");
                continue;
            }

            printf("Breakpoints:\n");
            int i;
            unsigned int flags;
            for (i = 0; i < num_breakpoints; i++) {
                flags = breakpoints[i].flags;
                printf("[%d] 0x%08X [%c%c%c]",
                       i, breakpoints[i].address,
                       flags & M64P_BKP_FLAG_READ ? 'R' : ' ',
                       flags & M64P_BKP_FLAG_WRITE ? 'W' : ' ',
                       flags & M64P_BKP_FLAG_EXEC ? 'X' : ' ');
                if ((breakpoints[i].flags & M64P_BKP_FLAG_ENABLED) == 0)
                    printf(" (Disabled)");
                printf("\n");
            }
        }
        else if (strncmp(input, "bp add ", 7) == 0) {
            unsigned int value = 0;
            if (strcmp(input, "bp add pc") == 0)
                value = cur_pc;
            else if (strncmp(input, "bp add 0x", 9) == 0) {
                sscanf(input, "bp add 0x%x", &value);
                if (value == 0)
                    sscanf(input, "bp add 0x%X", &value);
            }
            else {
                 sscanf(input, "bp add %x", &value);
                 if (value == 0)
                     sscanf(input, "bp add %X", &value);
            }

            if (value == 0) {
                printf("Invalid breakpoint address.\n");
                continue;
            }

            m64p_breakpoint bkpt;
            bkpt.address = value;
            bkpt.endaddr = value;
            bkpt.flags = M64P_BKP_FLAG_ENABLED |
                         M64P_BKP_FLAG_READ |
                         M64P_BKP_FLAG_WRITE |
                         M64P_BKP_FLAG_EXEC |
                         M64P_BKP_FLAG_LOG;
            int numBkps =
                (*DebugBreakpointCommand)(M64P_BKP_CMD_ADD_STRUCT, 0, &bkpt);
            if (numBkps == -1) {
                printf("Maximum breakpoint limit already reached.\n");
                continue;
            }

            breakpoints[num_breakpoints] = bkpt;
            num_breakpoints++;
            printf("Added breakpoint at 0x%08X.\n", value);
        }
        else if (strncmp(input, "bp rm ", 6) == 0) {
            int index = -1;
            unsigned int addr = 0;
            if (strncmp(input, "bp rm 0x", 8) == 0) {
                sscanf(input, "bp rm 0x%X", &addr);
                if (addr == 0)
                    sscanf(input, "bp rm 0x%x", &addr);

                int i;
                for (i = 0; i < num_breakpoints; i++) {
                    if (breakpoints[i].address == addr)
                        index = i;
                }
            }
            else {
                sscanf(input, "bp rm %d", &index);
                if (index >= 0 && index < num_breakpoints)
                    addr = breakpoints[index].address;
            }

            if (index == -1 || addr == 0) {
                printf("Invalid value passed. ");
                printf("Pass an address or breakpoint number.\n");
                continue;
            }

            (*DebugBreakpointCommand)(M64P_BKP_CMD_REMOVE_IDX, index, NULL);
            num_breakpoints--;
            printf("Breakpoint [%d] 0x%08X removed.\n", index, addr);

            // Shift the array elements ahead of index down.
            int j;
            for (j = index + 1; j < num_breakpoints; j++) {
                breakpoints[j - 1] = breakpoints[j];
            }

        }
        else if (strcmp(input, "exit") == 0 || strcmp(input, "quit") == 0) {
            (*CoreDoCommand)(M64CMD_STOP, 0, NULL);
        }
        else if (strlen(input) == 0)
            continue;
        else
            printf("Unrecognized: %s\n", input);
    }

    return -1;
}

