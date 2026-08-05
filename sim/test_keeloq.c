/*
 * KEELOQ test vectors — validate implementation against li0ard/keeloq
 * These are exactly the vectors from the reference TypeScript library tests.
 */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "keeloq.h"

static int passed = 0, failed = 0;

static void check_u32(const char *name, uint32_t got, uint32_t expected) {
    if (got == expected) {
        printf("  ✓ %-40s got=0x%08X\n", name, got);
        passed++;
    } else {
        printf("  ✗ %-40s got=0x%08X expected=0x%08X\n", name, got, expected);
        failed++;
    }
}

static void check_u64(const char *name, uint64_t got, uint64_t expected) {
    if (got == expected) {
        printf("  ✓ %-40s got=0x%016" PRIX64 "\n", name, got);
        passed++;
    } else {
        printf("  ✗ %-40s got=0x%016" PRIX64 " expected=0x%016" PRIX64 "\n",
               name, got, expected);
        failed++;
    }
}

int main(void) {
    printf("=== KEELOQ Test Vectors (=from li0ard/keeloq reference) ===\n\n");

    /* Test 1: Simple learning
     * key = 0x123456789ABCDEFn (=63 bit truncated), hop = 0xf16c47a6
     * expected decrypt raw = 0x21eb000a
     */
    printf("[Simple learning]\n");
    uint64_t key1 = 0x123456789ABCDEFULL;
    uint32_t hop1 = 0xf16c47a6;
    uint32_t dec1 = keeloq_decrypt(hop1, key1);
    check_u32("decrypt(0xf16c47a6, key=0x123..)", dec1, 0x21eb000a);

    /* Test 2: Normal learning
     * fix = 0x1ee2b949, key = 0x123456789ABCDEFn
     * expected man (=derived crypt_key) = 0xd107d43929d4778n
     */
    printf("\n[Normal learning]\n");
    uint64_t man2 = keeloq_normal_learning(0x1ee2b949, key1);
    check_u64("normal_learning(serial=0x1ee2b949)", man2, 0x0d107d43929d4778ULL);

    /* Then decrypt hop 0xc2be08b0 with the derived key */
    uint32_t hop2 = 0xc2be08b0;
    uint32_t dec2 = keeloq_decrypt(hop2, man2);
    check_u32("decrypt(0xc2be08b0, normal_key)", dec2, 0x11490006);

    /* Test 3: Secure learning
     * fix=0x11111111, seed=0x22222222, key=0x123..
     * expected man = 0xf2d8c1b388888888n
     */
    printf("\n[Secure learning]\n");
    uint64_t man3 = keeloq_secure_learning(0x11111111, 0x22222222, key1);
    check_u64("secure_learning(0x11..., seed=0x22...)", man3, 0xf2d8c1b388888888ULL);

    /* Test 4: encrypt/decrypt round-trip */
    printf("\n[Round-trip encrypt/decrypt]\n");
    uint32_t original = 0xDEADBEEF;
    uint32_t enc = keeloq_encrypt(original, key1);
    uint32_t dec = keeloq_decrypt(enc, key1);
    check_u32("decrypt(encrypt(0xDEADBEEF))", dec, original);

    printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
