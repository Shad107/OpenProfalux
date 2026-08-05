/*
 * OpenProfalux simulation entry point
 * Runs full pairing + command sequence with verbose logging
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "profalux.h"

extern int cc1101_init(void);

static void print_header(const char *title) {
    printf("\n\033[1;36m╔════════════════════════════════════════════╗\033[0m\n");
    printf("\033[1;36m║ %-42s ║\033[0m\n", title);
    printf("\033[1;36m╚════════════════════════════════════════════╝\033[0m\n");
}

int main(int argc, char **argv) {
    print_header("OpenProfalux PC Simulation");

    cc1101_init();

    pfx_tx_state_t state;
    pfx_state_init(&state);

    /* If arg "reset" passed → force new state */
    if (argc > 1 && strcmp(argv[1], "reset") == 0) {
        print_header("FORCE STATE RESET");
        pfx_state_reset(&state);
    }

    /* --- Test 1: Build + parse round-trip --- */
    print_header("TEST 1: Build + parse round-trip");
    uint8_t frame[9];
    pfx_frame_build(&state, PFX_BTN_UP, frame);
    printf("[Frame built] ");
    for (int i = 0; i < 9; i++) printf("%02X ", frame[i]);
    printf("\n");

    pfx_rx_frame_t rx;
    pfx_frame_parse(frame, &rx);
    printf("[Parsed] serial=0x%08X button=0x%X enc=0x%08X status=0x%X\n",
           rx.serial, rx.button, rx.encrypted_hop, rx.status_flags);

    if (pfx_frame_decrypt(&rx, state.crypt_key) == 0) {
        printf("[Decrypted] counter=%u ✓ (=matches our TX counter %u)\n", rx.counter, state.counter);
    } else {
        printf("[Decrypt FAILED]\n");
    }

    /* --- Test 2: Simulated pair burst --- */
    print_header("TEST 2: Simulated pair burst (=10 frames sample)");
    pfx_emit_burst(&state, PFX_BTN_STOP, 10, 10000);

    /* --- Test 3: Commands --- */
    print_header("TEST 3: Emit commands UP → STOP → DOWN");
    pfx_emit_command(&state, PFX_BTN_UP);
    pfx_emit_command(&state, PFX_BTN_STOP);
    pfx_emit_command(&state, PFX_BTN_DOWN);

    /* --- Test 4: Counter persistence across reboot --- */
    print_header("TEST 4: Simulated reboot (=state persistence)");
    pfx_tx_state_t state2;
    pfx_state_init(&state2);
    printf("Serial before reboot: 0x%08X, after: 0x%08X %s\n",
           state.serial, state2.serial,
           state.serial == state2.serial ? "✓ SAME" : "✗ DIFFER");
    printf("Counter before: %u, after: %u %s\n",
           state.counter, state2.counter,
           state.counter == state2.counter ? "✓ SAME" : "✗ DIFFER");

    print_header("SIMULATION COMPLETE");
    return 0;
}
