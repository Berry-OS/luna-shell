/* Known-answer tests for the crypto inside airplay.h. */
#define AIRPLAY_IMPLEMENTATION
#include "../airplay.h"
#include <stdio.h>
static int fails;
static void hex(const char* h, uint8_t* o) { for (; h[0] && h[1]; h += 2) { unsigned v; sscanf(h, "%2x", &v); *o++ = (uint8_t)v; } }
static void check(const char* name, const uint8_t* got, const char* want_hex, size_t n) {
    uint8_t w[256]; hex(want_hex, w);
    int ok = memcmp(got, w, n) == 0;
    printf("%-28s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}
int main(void) {
    uint8_t o[64];
    ap_sha512(o, (const uint8_t*)"abc", 3);
    check("sha512(abc)", o, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", 64);
    ap_sha512(o, (const uint8_t*)"", 0);
    check("sha512(empty)", o, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e", 64);
    uint8_t m[200]; for (int i = 0; i < 200; i++) m[i] = (uint8_t)i;
    ap_sha512(o, m, 112);   /* crosses the one-block padding boundary */
    ap_sha512(o + 0, m, 112);
    /* RFC 7748 6.1 */
    uint8_t a[32], b[32], pa[32], pb[32], s1[32], s2[32];
    hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a);
    hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b);
    ap_x25519_base(pa, a); ap_x25519_base(pb, b);
    check("x25519 pub A", pa, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", 32);
    check("x25519 pub B", pb, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", 32);
    ap_x25519(s1, a, pb); ap_x25519(s2, b, pa);
    check("x25519 shared (A)", s1, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32);
    check("x25519 shared (B)", s2, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32);
    /* RFC 8032 7.1 TEST 1 (empty message) and TEST 2 */
    uint8_t seed[32], pk[32], sig[64];
    hex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed);
    ap_ed25519_pubkey(pk, seed);
    check("ed25519 pk (t1)", pk, "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", 32);
    ap_ed25519_sign(sig, (const uint8_t*)"", 0, seed);
    check("ed25519 sig (t1)", sig, "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", 64);
    printf("%-28s %s\n", "ed25519 verify (t1)", ap_ed25519_verify(sig, (const uint8_t*)"", 0, pk) ? "ok" : (fails++, "FAIL"));
    uint8_t bad[64]; memcpy(bad, sig, 64); bad[5] ^= 1;
    printf("%-28s %s\n", "ed25519 reject tampered", !ap_ed25519_verify(bad, (const uint8_t*)"", 0, pk) ? "ok" : (fails++, "FAIL"));
    hex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", seed);
    ap_ed25519_pubkey(pk, seed);
    check("ed25519 pk (t3)", pk, "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", 32);
    uint8_t m3[2] = { 0x72 };
    ap_ed25519_sign(sig, m3, 1, seed);
    check("ed25519 sig (t3)", sig, "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00", 64);
    printf("%-28s %s\n", "ed25519 verify (t3)", ap_ed25519_verify(sig, m3, 1, pk) ? "ok" : (fails++, "FAIL"));
    /* FIPS-197 C.1 */
    uint8_t key[16], pt[16], ct[16], dt[16]; ap_aes128 c;
    hex("000102030405060708090a0b0c0d0e0f", key); hex("00112233445566778899aabbccddeeff", pt);
    ap_aes128_init(&c, key); ap_aes128_encrypt_block(&c, pt, ct);
    check("aes128 encrypt", ct, "69c4e0d86a7b0430d8cdb78070b4c55a", 16);
    ap_aes128_decrypt_block(&c, ct, dt);
    check("aes128 decrypt", dt, "00112233445566778899aabbccddeeff", 16);
    /* NIST SP 800-38A F.5.1 CTR-AES128 */
    uint8_t k2[16], iv[16], p4[32], c4[32];
    hex("2b7e151628aed2a6abf7158809cf4f3c", k2); hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", iv);
    hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51", p4);
    ap_aes_ctr x; ap_aes_ctr_init(&x, k2, iv);
    ap_aes_ctr_xor(&x, p4, c4, 7); ap_aes_ctr_xor(&x, p4 + 7, c4 + 7, 25);   /* split call: keystream must carry over */
    check("aes128 ctr (split)", c4, "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff", 32);
    /* NIST SP 800-38A F.2.2 CBC-AES128 decrypt */
    uint8_t iv2[16], cc[32], pp[32];
    hex("000102030405060708090a0b0c0d0e0f", iv2);
    hex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2", cc);
    ap_aes128_init(&c, k2); ap_aes128_cbc_decrypt(&c, iv2, cc, pp, 32);
    check("aes128 cbc decrypt", pp, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51", 32);
    printf("%s (%d failures)\n", fails ? "FAILED" : "ALL OK", fails);
    return fails != 0;
}
