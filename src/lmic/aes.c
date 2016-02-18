/*******************************************************************************
 * Copyright (c) 2014-2015 IBM Corporation.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * which accompanies this distribution, and is available at
 * http://www.eclipse.org/legal/epl-v10.html
 *
 * Contributors:
 *    IBM Zurich Research Lab - initial API, implementation and documentation
 *******************************************************************************/

#include "oslmic.h"
#include <Arduino.h>
#include <stdio.h>

u4_t AESAUX[16/sizeof(u4_t)];
u4_t AESKEY[11*16/sizeof(u4_t)];

#if LMIC_AES_IMPLEMENTATION > 1
#if LMIC_AES_IMPLEMENTATION == 2
#include "../aes/AES-128_V10.h"
#elif LMIC_AES_IMPLEMENTATION == 3
#include "../tiny-aes128-c/aes.h"
#define AES_Encrypt(data, key) AES128_ECB_encrypt(data, key, data)
#elif LMIC_AES_IMPLEMENTATION == 4
#include <aes128_enc.h>
#define AES_Encrypt(data, key) aes128_enc_single(key, data)
#elif LMIC_AES_IMPLEMENTATION == 5
#include "../aes-min/aes.h"
#define AES_Encrypt(data, key) do {u1_t tmp_key[16]; memcpy(tmp_key, key, 16); aes128_otfks_encrypt(data, tmp_key); } while (0)
#elif LMIC_AES_IMPLEMENTATION == 6
#include "../aes-min/aes.h"
#define AES_Encrypt(data, key) aes128_encrypt(data, key_schedule)
static u1_t key_schedule[AES128_KEY_SCHEDULE_SIZE];
#elif LMIC_AES_IMPLEMENTATION == 7
#include "../avr-aes/aes.h"
#if (AES_IMPLEMENTATION == 2 || AES_IMPLEMENTATION == 3) // FAST and FURIOUS
static u1_t expanded_key[176];
#define AES_Encrypt(data, key) aesCipher(expanded_key, data)
#elif AES_IMPLEMENTATION == 4 // MINI
// Keeps key unchanged
#define AES_Encrypt(data, key) aesCipher(key, data)
#else // FANTASTIC and SMALL
// Key is changed
#define AES_Encrypt(data, key) do {u1_t tmp_key[16]; memcpy(tmp_key, key, 16); aesCipher(tmp_key, data); } while (0)
#endif
#endif

// Shift the given buffer left one bit
static void shift_left(xref2u1_t buf, u1_t len) {
    while (len--) {
        u1_t next = len ? buf[1] : 0;

        u1_t val = (*buf << 1);
        if (next & 0x80)
            val |= 1;
        *buf++ = val;
    }
}

// Apply RFC4493 CMAC, using AESKEY as the key. If prepend_aux is true,
// AESAUX is prepended to the message. AESAUX is used as working memory
// in any case. The CMAC result is returned in AESAUX as well.
static void os_aes_cmac(xref2u1_t buf, u2_t len, u1_t prepend_aux) {
    if (prepend_aux)
        AES_Encrypt(AESaux, AESkey);
    else
        memset (AESaux, 0, 16);

    while (len > 0) {
        u1_t need_padding = 0;
        for (u1_t i = 0; i < 16; ++i, ++buf, --len) {
            if (len == 0) {
                // The message is padded with 0x80 and then zeroes.
                // Since zeroes are no-op for xorg, we can just skip them
                // and leave AESAUX unchanged for them.
                AESaux[i] ^= 0x80;
                need_padding = 1;
                break;
            }
            AESaux[i] ^= *buf;
        }

        if (len == 0) {
            // Final block, xor with K1 or K2. K1 and K2 are calculated
            // by encrypting the all-zeroes block and then applying some
            // shifts and xor on that.
            u1_t final_key[16];
            memset(final_key, 0, sizeof(final_key));
            AES_Encrypt(final_key, AESkey);

            // Calculate K1
            u1_t msb = final_key[0] & 0x80;
            shift_left(final_key, sizeof(final_key));
            if (msb)
                final_key[sizeof(final_key)-1] ^= 0x87;

            // If the final block was not complete, calculate K2 from K1
            if (need_padding) {
                msb = final_key[0] & 0x80;
                shift_left(final_key, sizeof(final_key));
                if (msb)
                    final_key[sizeof(final_key)-1] ^= 0x87;
            }

            // Xor with K1 or K2
            for (u1_t i = 0; i < sizeof(final_key); ++i)
                AESaux[i] ^= final_key[i];
        }

        AES_Encrypt(AESaux, AESkey);
    }
}

// Run AES-CTR using the key in AESKEY and using AESAUX as the
// counter block. The last byte of the counter block will be incremented
// for every block. The given buffer will be encrypted in place.
static void os_aes_ctr (xref2u1_t buf, u2_t len) {
    u1_t ctr[16];
    while (len) {
        // Encrypt the counter block with the selected key
        memcpy(ctr, AESaux, sizeof(ctr));
        AES_Encrypt(ctr, AESkey);

        // Xor the payload with the resulting ciphertext
        for (u1_t i = 0; i < 16 && len > 0; i++, len--, buf++)
            *buf ^= ctr[i];

        // Increment the block index byte
        AESaux[15]++;
    }
}

u4_t os_aes_internal (u1_t mode, xref2u1_t buf, u2_t len) {
    switch (mode & ~AES_MICNOAUX) {
        case AES_MIC:
            os_aes_cmac(buf, len, /* prepend_aux */ !(mode & AES_MICNOAUX));
            return os_rmsbf4(AESaux);

        case AES_ENC:
            // TODO: This assumes len = 16. When is this used?
            AES_Encrypt(buf, AESkey);
            break;

        case AES_CTR:
            os_aes_ctr(buf, len);
            break;
    }
    return 0;
}

#elif LMIC_AES_IMPLEMENTATION == 1

#define AES_MICSUB 0x30 // internal use only

static CONST_TABLE(u4_t, AES_RCON)[10] = {
    0x01000000, 0x02000000, 0x04000000, 0x08000000, 0x10000000,
    0x20000000, 0x40000000, 0x80000000, 0x1B000000, 0x36000000
};

static CONST_TABLE(u1_t, AES_S)[256] = {
  0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
  0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
  0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
  0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
  0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
  0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
  0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
  0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
  0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
  0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
  0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
  0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
  0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
  0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
  0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
  0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
};

static CONST_TABLE(u4_t, AES_E1)[256] = {
  0xC66363A5, 0xF87C7C84, 0xEE777799, 0xF67B7B8D, 0xFFF2F20D, 0xD66B6BBD, 0xDE6F6FB1, 0x91C5C554,
  0x60303050, 0x02010103, 0xCE6767A9, 0x562B2B7D, 0xE7FEFE19, 0xB5D7D762, 0x4DABABE6, 0xEC76769A,
  0x8FCACA45, 0x1F82829D, 0x89C9C940, 0xFA7D7D87, 0xEFFAFA15, 0xB25959EB, 0x8E4747C9, 0xFBF0F00B,
  0x41ADADEC, 0xB3D4D467, 0x5FA2A2FD, 0x45AFAFEA, 0x239C9CBF, 0x53A4A4F7, 0xE4727296, 0x9BC0C05B,
  0x75B7B7C2, 0xE1FDFD1C, 0x3D9393AE, 0x4C26266A, 0x6C36365A, 0x7E3F3F41, 0xF5F7F702, 0x83CCCC4F,
  0x6834345C, 0x51A5A5F4, 0xD1E5E534, 0xF9F1F108, 0xE2717193, 0xABD8D873, 0x62313153, 0x2A15153F,
  0x0804040C, 0x95C7C752, 0x46232365, 0x9DC3C35E, 0x30181828, 0x379696A1, 0x0A05050F, 0x2F9A9AB5,
  0x0E070709, 0x24121236, 0x1B80809B, 0xDFE2E23D, 0xCDEBEB26, 0x4E272769, 0x7FB2B2CD, 0xEA75759F,
  0x1209091B, 0x1D83839E, 0x582C2C74, 0x341A1A2E, 0x361B1B2D, 0xDC6E6EB2, 0xB45A5AEE, 0x5BA0A0FB,
  0xA45252F6, 0x763B3B4D, 0xB7D6D661, 0x7DB3B3CE, 0x5229297B, 0xDDE3E33E, 0x5E2F2F71, 0x13848497,
  0xA65353F5, 0xB9D1D168, 0x00000000, 0xC1EDED2C, 0x40202060, 0xE3FCFC1F, 0x79B1B1C8, 0xB65B5BED,
  0xD46A6ABE, 0x8DCBCB46, 0x67BEBED9, 0x7239394B, 0x944A4ADE, 0x984C4CD4, 0xB05858E8, 0x85CFCF4A,
  0xBBD0D06B, 0xC5EFEF2A, 0x4FAAAAE5, 0xEDFBFB16, 0x864343C5, 0x9A4D4DD7, 0x66333355, 0x11858594,
  0x8A4545CF, 0xE9F9F910, 0x04020206, 0xFE7F7F81, 0xA05050F0, 0x783C3C44, 0x259F9FBA, 0x4BA8A8E3,
  0xA25151F3, 0x5DA3A3FE, 0x804040C0, 0x058F8F8A, 0x3F9292AD, 0x219D9DBC, 0x70383848, 0xF1F5F504,
  0x63BCBCDF, 0x77B6B6C1, 0xAFDADA75, 0x42212163, 0x20101030, 0xE5FFFF1A, 0xFDF3F30E, 0xBFD2D26D,
  0x81CDCD4C, 0x180C0C14, 0x26131335, 0xC3ECEC2F, 0xBE5F5FE1, 0x359797A2, 0x884444CC, 0x2E171739,
  0x93C4C457, 0x55A7A7F2, 0xFC7E7E82, 0x7A3D3D47, 0xC86464AC, 0xBA5D5DE7, 0x3219192B, 0xE6737395,
  0xC06060A0, 0x19818198, 0x9E4F4FD1, 0xA3DCDC7F, 0x44222266, 0x542A2A7E, 0x3B9090AB, 0x0B888883,
  0x8C4646CA, 0xC7EEEE29, 0x6BB8B8D3, 0x2814143C, 0xA7DEDE79, 0xBC5E5EE2, 0x160B0B1D, 0xADDBDB76,
  0xDBE0E03B, 0x64323256, 0x743A3A4E, 0x140A0A1E, 0x924949DB, 0x0C06060A, 0x4824246C, 0xB85C5CE4,
  0x9FC2C25D, 0xBDD3D36E, 0x43ACACEF, 0xC46262A6, 0x399191A8, 0x319595A4, 0xD3E4E437, 0xF279798B,
  0xD5E7E732, 0x8BC8C843, 0x6E373759, 0xDA6D6DB7, 0x018D8D8C, 0xB1D5D564, 0x9C4E4ED2, 0x49A9A9E0,
  0xD86C6CB4, 0xAC5656FA, 0xF3F4F407, 0xCFEAEA25, 0xCA6565AF, 0xF47A7A8E, 0x47AEAEE9, 0x10080818,
  0x6FBABAD5, 0xF0787888, 0x4A25256F, 0x5C2E2E72, 0x381C1C24, 0x57A6A6F1, 0x73B4B4C7, 0x97C6C651,
  0xCBE8E823, 0xA1DDDD7C, 0xE874749C, 0x3E1F1F21, 0x964B4BDD, 0x61BDBDDC, 0x0D8B8B86, 0x0F8A8A85,
  0xE0707090, 0x7C3E3E42, 0x71B5B5C4, 0xCC6666AA, 0x904848D8, 0x06030305, 0xF7F6F601, 0x1C0E0E12,
  0xC26161A3, 0x6A35355F, 0xAE5757F9, 0x69B9B9D0, 0x17868691, 0x99C1C158, 0x3A1D1D27, 0x279E9EB9,
  0xD9E1E138, 0xEBF8F813, 0x2B9898B3, 0x22111133, 0xD26969BB, 0xA9D9D970, 0x078E8E89, 0x339494A7,
  0x2D9B9BB6, 0x3C1E1E22, 0x15878792, 0xC9E9E920, 0x87CECE49, 0xAA5555FF, 0x50282878, 0xA5DFDF7A,
  0x038C8C8F, 0x59A1A1F8, 0x09898980, 0x1A0D0D17, 0x65BFBFDA, 0xD7E6E631, 0x844242C6, 0xD06868B8,
  0x824141C3, 0x299999B0, 0x5A2D2D77, 0x1E0F0F11, 0x7BB0B0CB, 0xA85454FC, 0x6DBBBBD6, 0x2C16163A,
};

static CONST_TABLE(u4_t, AES_E2)[256] = {
  0xA5C66363, 0x84F87C7C, 0x99EE7777, 0x8DF67B7B, 0x0DFFF2F2, 0xBDD66B6B, 0xB1DE6F6F, 0x5491C5C5,
  0x50603030, 0x03020101, 0xA9CE6767, 0x7D562B2B, 0x19E7FEFE, 0x62B5D7D7, 0xE64DABAB, 0x9AEC7676,
  0x458FCACA, 0x9D1F8282, 0x4089C9C9, 0x87FA7D7D, 0x15EFFAFA, 0xEBB25959, 0xC98E4747, 0x0BFBF0F0,
  0xEC41ADAD, 0x67B3D4D4, 0xFD5FA2A2, 0xEA45AFAF, 0xBF239C9C, 0xF753A4A4, 0x96E47272, 0x5B9BC0C0,
  0xC275B7B7, 0x1CE1FDFD, 0xAE3D9393, 0x6A4C2626, 0x5A6C3636, 0x417E3F3F, 0x02F5F7F7, 0x4F83CCCC,
  0x5C683434, 0xF451A5A5, 0x34D1E5E5, 0x08F9F1F1, 0x93E27171, 0x73ABD8D8, 0x53623131, 0x3F2A1515,
  0x0C080404, 0x5295C7C7, 0x65462323, 0x5E9DC3C3, 0x28301818, 0xA1379696, 0x0F0A0505, 0xB52F9A9A,
  0x090E0707, 0x36241212, 0x9B1B8080, 0x3DDFE2E2, 0x26CDEBEB, 0x694E2727, 0xCD7FB2B2, 0x9FEA7575,
  0x1B120909, 0x9E1D8383, 0x74582C2C, 0x2E341A1A, 0x2D361B1B, 0xB2DC6E6E, 0xEEB45A5A, 0xFB5BA0A0,
  0xF6A45252, 0x4D763B3B, 0x61B7D6D6, 0xCE7DB3B3, 0x7B522929, 0x3EDDE3E3, 0x715E2F2F, 0x97138484,
  0xF5A65353, 0x68B9D1D1, 0x00000000, 0x2CC1EDED, 0x60402020, 0x1FE3FCFC, 0xC879B1B1, 0xEDB65B5B,
  0xBED46A6A, 0x468DCBCB, 0xD967BEBE, 0x4B723939, 0xDE944A4A, 0xD4984C4C, 0xE8B05858, 0x4A85CFCF,
  0x6BBBD0D0, 0x2AC5EFEF, 0xE54FAAAA, 0x16EDFBFB, 0xC5864343, 0xD79A4D4D, 0x55663333, 0x94118585,
  0xCF8A4545, 0x10E9F9F9, 0x06040202, 0x81FE7F7F, 0xF0A05050, 0x44783C3C, 0xBA259F9F, 0xE34BA8A8,
  0xF3A25151, 0xFE5DA3A3, 0xC0804040, 0x8A058F8F, 0xAD3F9292, 0xBC219D9D, 0x48703838, 0x04F1F5F5,
  0xDF63BCBC, 0xC177B6B6, 0x75AFDADA, 0x63422121, 0x30201010, 0x1AE5FFFF, 0x0EFDF3F3, 0x6DBFD2D2,
  0x4C81CDCD, 0x14180C0C, 0x35261313, 0x2FC3ECEC, 0xE1BE5F5F, 0xA2359797, 0xCC884444, 0x392E1717,
  0x5793C4C4, 0xF255A7A7, 0x82FC7E7E, 0x477A3D3D, 0xACC86464, 0xE7BA5D5D, 0x2B321919, 0x95E67373,
  0xA0C06060, 0x98198181, 0xD19E4F4F, 0x7FA3DCDC, 0x66442222, 0x7E542A2A, 0xAB3B9090, 0x830B8888,
  0xCA8C4646, 0x29C7EEEE, 0xD36BB8B8, 0x3C281414, 0x79A7DEDE, 0xE2BC5E5E, 0x1D160B0B, 0x76ADDBDB,
  0x3BDBE0E0, 0x56643232, 0x4E743A3A, 0x1E140A0A, 0xDB924949, 0x0A0C0606, 0x6C482424, 0xE4B85C5C,
  0x5D9FC2C2, 0x6EBDD3D3, 0xEF43ACAC, 0xA6C46262, 0xA8399191, 0xA4319595, 0x37D3E4E4, 0x8BF27979,
  0x32D5E7E7, 0x438BC8C8, 0x596E3737, 0xB7DA6D6D, 0x8C018D8D, 0x64B1D5D5, 0xD29C4E4E, 0xE049A9A9,
  0xB4D86C6C, 0xFAAC5656, 0x07F3F4F4, 0x25CFEAEA, 0xAFCA6565, 0x8EF47A7A, 0xE947AEAE, 0x18100808,
  0xD56FBABA, 0x88F07878, 0x6F4A2525, 0x725C2E2E, 0x24381C1C, 0xF157A6A6, 0xC773B4B4, 0x5197C6C6,
  0x23CBE8E8, 0x7CA1DDDD, 0x9CE87474, 0x213E1F1F, 0xDD964B4B, 0xDC61BDBD, 0x860D8B8B, 0x850F8A8A,
  0x90E07070, 0x427C3E3E, 0xC471B5B5, 0xAACC6666, 0xD8904848, 0x05060303, 0x01F7F6F6, 0x121C0E0E,
  0xA3C26161, 0x5F6A3535, 0xF9AE5757, 0xD069B9B9, 0x91178686, 0x5899C1C1, 0x273A1D1D, 0xB9279E9E,
  0x38D9E1E1, 0x13EBF8F8, 0xB32B9898, 0x33221111, 0xBBD26969, 0x70A9D9D9, 0x89078E8E, 0xA7339494,
  0xB62D9B9B, 0x223C1E1E, 0x92158787, 0x20C9E9E9, 0x4987CECE, 0xFFAA5555, 0x78502828, 0x7AA5DFDF,
  0x8F038C8C, 0xF859A1A1, 0x80098989, 0x171A0D0D, 0xDA65BFBF, 0x31D7E6E6, 0xC6844242, 0xB8D06868,
  0xC3824141, 0xB0299999, 0x775A2D2D, 0x111E0F0F, 0xCB7BB0B0, 0xFCA85454, 0xD66DBBBB, 0x3A2C1616,
};

static CONST_TABLE(u4_t, AES_E3)[256] = {
  0x63A5C663, 0x7C84F87C, 0x7799EE77, 0x7B8DF67B, 0xF20DFFF2, 0x6BBDD66B, 0x6FB1DE6F, 0xC55491C5,
  0x30506030, 0x01030201, 0x67A9CE67, 0x2B7D562B, 0xFE19E7FE, 0xD762B5D7, 0xABE64DAB, 0x769AEC76,
  0xCA458FCA, 0x829D1F82, 0xC94089C9, 0x7D87FA7D, 0xFA15EFFA, 0x59EBB259, 0x47C98E47, 0xF00BFBF0,
  0xADEC41AD, 0xD467B3D4, 0xA2FD5FA2, 0xAFEA45AF, 0x9CBF239C, 0xA4F753A4, 0x7296E472, 0xC05B9BC0,
  0xB7C275B7, 0xFD1CE1FD, 0x93AE3D93, 0x266A4C26, 0x365A6C36, 0x3F417E3F, 0xF702F5F7, 0xCC4F83CC,
  0x345C6834, 0xA5F451A5, 0xE534D1E5, 0xF108F9F1, 0x7193E271, 0xD873ABD8, 0x31536231, 0x153F2A15,
  0x040C0804, 0xC75295C7, 0x23654623, 0xC35E9DC3, 0x18283018, 0x96A13796, 0x050F0A05, 0x9AB52F9A,
  0x07090E07, 0x12362412, 0x809B1B80, 0xE23DDFE2, 0xEB26CDEB, 0x27694E27, 0xB2CD7FB2, 0x759FEA75,
  0x091B1209, 0x839E1D83, 0x2C74582C, 0x1A2E341A, 0x1B2D361B, 0x6EB2DC6E, 0x5AEEB45A, 0xA0FB5BA0,
  0x52F6A452, 0x3B4D763B, 0xD661B7D6, 0xB3CE7DB3, 0x297B5229, 0xE33EDDE3, 0x2F715E2F, 0x84971384,
  0x53F5A653, 0xD168B9D1, 0x00000000, 0xED2CC1ED, 0x20604020, 0xFC1FE3FC, 0xB1C879B1, 0x5BEDB65B,
  0x6ABED46A, 0xCB468DCB, 0xBED967BE, 0x394B7239, 0x4ADE944A, 0x4CD4984C, 0x58E8B058, 0xCF4A85CF,
  0xD06BBBD0, 0xEF2AC5EF, 0xAAE54FAA, 0xFB16EDFB, 0x43C58643, 0x4DD79A4D, 0x33556633, 0x85941185,
  0x45CF8A45, 0xF910E9F9, 0x02060402, 0x7F81FE7F, 0x50F0A050, 0x3C44783C, 0x9FBA259F, 0xA8E34BA8,
  0x51F3A251, 0xA3FE5DA3, 0x40C08040, 0x8F8A058F, 0x92AD3F92, 0x9DBC219D, 0x38487038, 0xF504F1F5,
  0xBCDF63BC, 0xB6C177B6, 0xDA75AFDA, 0x21634221, 0x10302010, 0xFF1AE5FF, 0xF30EFDF3, 0xD26DBFD2,
  0xCD4C81CD, 0x0C14180C, 0x13352613, 0xEC2FC3EC, 0x5FE1BE5F, 0x97A23597, 0x44CC8844, 0x17392E17,
  0xC45793C4, 0xA7F255A7, 0x7E82FC7E, 0x3D477A3D, 0x64ACC864, 0x5DE7BA5D, 0x192B3219, 0x7395E673,
  0x60A0C060, 0x81981981, 0x4FD19E4F, 0xDC7FA3DC, 0x22664422, 0x2A7E542A, 0x90AB3B90, 0x88830B88,
  0x46CA8C46, 0xEE29C7EE, 0xB8D36BB8, 0x143C2814, 0xDE79A7DE, 0x5EE2BC5E, 0x0B1D160B, 0xDB76ADDB,
  0xE03BDBE0, 0x32566432, 0x3A4E743A, 0x0A1E140A, 0x49DB9249, 0x060A0C06, 0x246C4824, 0x5CE4B85C,
  0xC25D9FC2, 0xD36EBDD3, 0xACEF43AC, 0x62A6C462, 0x91A83991, 0x95A43195, 0xE437D3E4, 0x798BF279,
  0xE732D5E7, 0xC8438BC8, 0x37596E37, 0x6DB7DA6D, 0x8D8C018D, 0xD564B1D5, 0x4ED29C4E, 0xA9E049A9,
  0x6CB4D86C, 0x56FAAC56, 0xF407F3F4, 0xEA25CFEA, 0x65AFCA65, 0x7A8EF47A, 0xAEE947AE, 0x08181008,
  0xBAD56FBA, 0x7888F078, 0x256F4A25, 0x2E725C2E, 0x1C24381C, 0xA6F157A6, 0xB4C773B4, 0xC65197C6,
  0xE823CBE8, 0xDD7CA1DD, 0x749CE874, 0x1F213E1F, 0x4BDD964B, 0xBDDC61BD, 0x8B860D8B, 0x8A850F8A,
  0x7090E070, 0x3E427C3E, 0xB5C471B5, 0x66AACC66, 0x48D89048, 0x03050603, 0xF601F7F6, 0x0E121C0E,
  0x61A3C261, 0x355F6A35, 0x57F9AE57, 0xB9D069B9, 0x86911786, 0xC15899C1, 0x1D273A1D, 0x9EB9279E,
  0xE138D9E1, 0xF813EBF8, 0x98B32B98, 0x11332211, 0x69BBD269, 0xD970A9D9, 0x8E89078E, 0x94A73394,
  0x9BB62D9B, 0x1E223C1E, 0x87921587, 0xE920C9E9, 0xCE4987CE, 0x55FFAA55, 0x28785028, 0xDF7AA5DF,
  0x8C8F038C, 0xA1F859A1, 0x89800989, 0x0D171A0D, 0xBFDA65BF, 0xE631D7E6, 0x42C68442, 0x68B8D068,
  0x41C38241, 0x99B02999, 0x2D775A2D, 0x0F111E0F, 0xB0CB7BB0, 0x54FCA854, 0xBBD66DBB, 0x163A2C16,
};

static CONST_TABLE(u4_t, AES_E4)[256] = {
  0x6363A5C6, 0x7C7C84F8, 0x777799EE, 0x7B7B8DF6, 0xF2F20DFF, 0x6B6BBDD6, 0x6F6FB1DE, 0xC5C55491,
  0x30305060, 0x01010302, 0x6767A9CE, 0x2B2B7D56, 0xFEFE19E7, 0xD7D762B5, 0xABABE64D, 0x76769AEC,
  0xCACA458F, 0x82829D1F, 0xC9C94089, 0x7D7D87FA, 0xFAFA15EF, 0x5959EBB2, 0x4747C98E, 0xF0F00BFB,
  0xADADEC41, 0xD4D467B3, 0xA2A2FD5F, 0xAFAFEA45, 0x9C9CBF23, 0xA4A4F753, 0x727296E4, 0xC0C05B9B,
  0xB7B7C275, 0xFDFD1CE1, 0x9393AE3D, 0x26266A4C, 0x36365A6C, 0x3F3F417E, 0xF7F702F5, 0xCCCC4F83,
  0x34345C68, 0xA5A5F451, 0xE5E534D1, 0xF1F108F9, 0x717193E2, 0xD8D873AB, 0x31315362, 0x15153F2A,
  0x04040C08, 0xC7C75295, 0x23236546, 0xC3C35E9D, 0x18182830, 0x9696A137, 0x05050F0A, 0x9A9AB52F,
  0x0707090E, 0x12123624, 0x80809B1B, 0xE2E23DDF, 0xEBEB26CD, 0x2727694E, 0xB2B2CD7F, 0x75759FEA,
  0x09091B12, 0x83839E1D, 0x2C2C7458, 0x1A1A2E34, 0x1B1B2D36, 0x6E6EB2DC, 0x5A5AEEB4, 0xA0A0FB5B,
  0x5252F6A4, 0x3B3B4D76, 0xD6D661B7, 0xB3B3CE7D, 0x29297B52, 0xE3E33EDD, 0x2F2F715E, 0x84849713,
  0x5353F5A6, 0xD1D168B9, 0x00000000, 0xEDED2CC1, 0x20206040, 0xFCFC1FE3, 0xB1B1C879, 0x5B5BEDB6,
  0x6A6ABED4, 0xCBCB468D, 0xBEBED967, 0x39394B72, 0x4A4ADE94, 0x4C4CD498, 0x5858E8B0, 0xCFCF4A85,
  0xD0D06BBB, 0xEFEF2AC5, 0xAAAAE54F, 0xFBFB16ED, 0x4343C586, 0x4D4DD79A, 0x33335566, 0x85859411,
  0x4545CF8A, 0xF9F910E9, 0x02020604, 0x7F7F81FE, 0x5050F0A0, 0x3C3C4478, 0x9F9FBA25, 0xA8A8E34B,
  0x5151F3A2, 0xA3A3FE5D, 0x4040C080, 0x8F8F8A05, 0x9292AD3F, 0x9D9DBC21, 0x38384870, 0xF5F504F1,
  0xBCBCDF63, 0xB6B6C177, 0xDADA75AF, 0x21216342, 0x10103020, 0xFFFF1AE5, 0xF3F30EFD, 0xD2D26DBF,
  0xCDCD4C81, 0x0C0C1418, 0x13133526, 0xECEC2FC3, 0x5F5FE1BE, 0x9797A235, 0x4444CC88, 0x1717392E,
  0xC4C45793, 0xA7A7F255, 0x7E7E82FC, 0x3D3D477A, 0x6464ACC8, 0x5D5DE7BA, 0x19192B32, 0x737395E6,
  0x6060A0C0, 0x81819819, 0x4F4FD19E, 0xDCDC7FA3, 0x22226644, 0x2A2A7E54, 0x9090AB3B, 0x8888830B,
  0x4646CA8C, 0xEEEE29C7, 0xB8B8D36B, 0x14143C28, 0xDEDE79A7, 0x5E5EE2BC, 0x0B0B1D16, 0xDBDB76AD,
  0xE0E03BDB, 0x32325664, 0x3A3A4E74, 0x0A0A1E14, 0x4949DB92, 0x06060A0C, 0x24246C48, 0x5C5CE4B8,
  0xC2C25D9F, 0xD3D36EBD, 0xACACEF43, 0x6262A6C4, 0x9191A839, 0x9595A431, 0xE4E437D3, 0x79798BF2,
  0xE7E732D5, 0xC8C8438B, 0x3737596E, 0x6D6DB7DA, 0x8D8D8C01, 0xD5D564B1, 0x4E4ED29C, 0xA9A9E049,
  0x6C6CB4D8, 0x5656FAAC, 0xF4F407F3, 0xEAEA25CF, 0x6565AFCA, 0x7A7A8EF4, 0xAEAEE947, 0x08081810,
  0xBABAD56F, 0x787888F0, 0x25256F4A, 0x2E2E725C, 0x1C1C2438, 0xA6A6F157, 0xB4B4C773, 0xC6C65197,
  0xE8E823CB, 0xDDDD7CA1, 0x74749CE8, 0x1F1F213E, 0x4B4BDD96, 0xBDBDDC61, 0x8B8B860D, 0x8A8A850F,
  0x707090E0, 0x3E3E427C, 0xB5B5C471, 0x6666AACC, 0x4848D890, 0x03030506, 0xF6F601F7, 0x0E0E121C,
  0x6161A3C2, 0x35355F6A, 0x5757F9AE, 0xB9B9D069, 0x86869117, 0xC1C15899, 0x1D1D273A, 0x9E9EB927,
  0xE1E138D9, 0xF8F813EB, 0x9898B32B, 0x11113322, 0x6969BBD2, 0xD9D970A9, 0x8E8E8907, 0x9494A733,
  0x9B9BB62D, 0x1E1E223C, 0x87879215, 0xE9E920C9, 0xCECE4987, 0x5555FFAA, 0x28287850, 0xDFDF7AA5,
  0x8C8C8F03, 0xA1A1F859, 0x89898009, 0x0D0D171A, 0xBFBFDA65, 0xE6E631D7, 0x4242C684, 0x6868B8D0,
  0x4141C382, 0x9999B029, 0x2D2D775A, 0x0F0F111E, 0xB0B0CB7B, 0x5454FCA8, 0xBBBBD66D, 0x16163A2C,
};

#define msbf4_read(p)    ((p)[0]<<24 | (p)[1]<<16 | (p)[2]<<8 | (p)[3])
#define msbf4_write(p,v) (p)[0]=(v)>>24,(p)[1]=(v)>>16,(p)[2]=(v)>>8,(p)[3]=(v)
#define swapmsbf(x)      ( (x&0xFF)<<24 | (x&0xFF00)<<8 | (x&0xFF0000)>>8 | (x>>24) )

#define u1(v)                       ((u1_t)(v))

#define AES_key4(r1,r2,r3,r0,i)    r1 = ki[i+1]; \
                                   r2 = ki[i+2]; \
                                   r3 = ki[i+3]; \
                                   r0 = ki[i]

#define AES_expr4(r1,r2,r3,r0,i)   r1 ^= TABLE_GET_U4(AES_E4, u1(i));     \
                                   r2 ^= TABLE_GET_U4(AES_E3, u1(i>>8));  \
                                   r3 ^= TABLE_GET_U4(AES_E2, u1(i>>16)); \
                                   r0 ^= TABLE_GET_U4(AES_E1,   (i>>24))

#define AES_expr(a,r0,r1,r2,r3,i)  a = ki[i];                    \
                                   a ^= ((u4_t)TABLE_GET_U1(AES_S,    r0>>24 )<<24); \
                                   a ^= ((u4_t)TABLE_GET_U1(AES_S, u1(r1>>16))<<16); \
                                   a ^= ((u4_t)TABLE_GET_U1(AES_S, u1(r2>> 8))<< 8); \
                                   a ^=  (u4_t)TABLE_GET_U1(AES_S, u1(r3)    )

// global area for passing parameters (aux, key) and for storing round keys
u4_t AESAUX[16/sizeof(u4_t)];
u4_t AESKEY[11*16/sizeof(u4_t)];

// generate 1+10 roundkeys for encryption with 128-bit key
// read 128-bit key from AESKEY in MSBF, generate roundkey words in place
static void aesroundkeys () {
    int i;
    u4_t b;

    for( i=0; i<4; i++) {
        AESKEY[i] = swapmsbf(AESKEY[i]);
    }

    b = AESKEY[3];
    for( ; i<44; i++ ) {
        if( i%4==0 ) {
            // b = SubWord(RotWord(b)) xor Rcon[i/4]
            b = ((u4_t)TABLE_GET_U1(AES_S, u1(b >> 16)) << 24) ^
                ((u4_t)TABLE_GET_U1(AES_S, u1(b >>  8)) << 16) ^
                ((u4_t)TABLE_GET_U1(AES_S, u1(b)      ) <<  8) ^
                ((u4_t)TABLE_GET_U1(AES_S,    b >> 24 )      ) ^
                 TABLE_GET_U4(AES_RCON, (i-4)/4);
        }
        AESKEY[i] = b ^= AESKEY[i-4];
    }
}

u4_t os_aes_internal (u1_t mode, xref2u1_t buf, u2_t len) {

        aesroundkeys();

        if( mode & AES_MICNOAUX ) {
            AESAUX[0] = AESAUX[1] = AESAUX[2] = AESAUX[3] = 0;
        } else {
            AESAUX[0] = swapmsbf(AESAUX[0]);
            AESAUX[1] = swapmsbf(AESAUX[1]);
            AESAUX[2] = swapmsbf(AESAUX[2]);
            AESAUX[3] = swapmsbf(AESAUX[3]);
        }

        while( (signed char)len > 0 ) {
            u4_t a0, a1, a2, a3;
            u4_t t0, t1, t2, t3;
            u4_t *ki, *ke;

            // load input block
            if( (mode & AES_CTR) || ((mode & AES_MIC) && (mode & AES_MICNOAUX)==0) ) { // load CTR block or first MIC block
                a0 = AESAUX[0];
                a1 = AESAUX[1];
                a2 = AESAUX[2];
                a3 = AESAUX[3];
            }
            else if( (mode & AES_MIC) && len <= 16 ) { // last MIC block
                a0 = a1 = a2 = a3 = 0; // load null block
                mode |= ((len == 16) ? 1 : 2) << 4; // set MICSUB: CMAC subkey K1 or K2
            } else
        LOADDATA: { // load data block (partially)
                for(t0=0; t0<16; t0++) {
                    t1 = (t1<<8) | ((t0<len) ? buf[t0] : (t0==len) ? 0x80 : 0x00);
                    if((t0&3)==3) {
                        a0 = a1;
                        a1 = a2;
                        a2 = a3;
                        a3 = t1;
                    }
                }
                if( mode & AES_MIC ) {
                    a0 ^= AESAUX[0];
                    a1 ^= AESAUX[1];
                    a2 ^= AESAUX[2];
                    a3 ^= AESAUX[3];
                }
            }

            // perform AES encryption on block in a0-a3
            ki = AESKEY;
            ke = ki + 8*4;
            a0 ^= ki[0];
            a1 ^= ki[1];
            a2 ^= ki[2];
            a3 ^= ki[3];
            do {
                AES_key4 (t1,t2,t3,t0,4);
                AES_expr4(t1,t2,t3,t0,a0);
                AES_expr4(t2,t3,t0,t1,a1);
                AES_expr4(t3,t0,t1,t2,a2);
                AES_expr4(t0,t1,t2,t3,a3);

                AES_key4 (a1,a2,a3,a0,8);
                AES_expr4(a1,a2,a3,a0,t0);
                AES_expr4(a2,a3,a0,a1,t1);
                AES_expr4(a3,a0,a1,a2,t2);
                AES_expr4(a0,a1,a2,a3,t3);
            } while( (ki+=8) < ke );

            AES_key4 (t1,t2,t3,t0,4);
            AES_expr4(t1,t2,t3,t0,a0);
            AES_expr4(t2,t3,t0,t1,a1);
            AES_expr4(t3,t0,t1,t2,a2);
            AES_expr4(t0,t1,t2,t3,a3);

            AES_expr(a0,t0,t1,t2,t3,8);
            AES_expr(a1,t1,t2,t3,t0,9);
            AES_expr(a2,t2,t3,t0,t1,10);
            AES_expr(a3,t3,t0,t1,t2,11);
            // result of AES encryption in a0-a3

            if( mode & AES_MIC ) {
                if( (t1 = (mode & AES_MICSUB) >> 4) != 0 ) { // last block
                    do {
                        // compute CMAC subkey K1 and K2
                        t0 = a0 >> 31; // save MSB
                        a0 = (a0 << 1) | (a1 >> 31);
                        a1 = (a1 << 1) | (a2 >> 31);
                        a2 = (a2 << 1) | (a3 >> 31);
                        a3 = (a3 << 1);
                        if( t0 ) a3 ^= 0x87;
                    } while( --t1 );

                    AESAUX[0] ^= a0;
                    AESAUX[1] ^= a1;
                    AESAUX[2] ^= a2;
                    AESAUX[3] ^= a3;
                    mode &= ~AES_MICSUB;
                    goto LOADDATA;
                } else {
                    // save cipher block as new iv
                    AESAUX[0] = a0;
                    AESAUX[1] = a1;
                    AESAUX[2] = a2;
                    AESAUX[3] = a3;
                }
            } else { // CIPHER
                if( mode & AES_CTR ) { // xor block (partially)
                    t0 = (len > 16) ? 16: len;
                    for(t1=0; t1<t0; t1++) {
                        buf[t1] ^= (a0>>24);
                        a0 <<= 8;
                        if((t1&3)==3) {
                            a0 = a1;
                            a1 = a2;
                            a2 = a3;
                        }
                    }
                    // update counter
                    AESAUX[3]++;
                } else { // ECB
                    // store block
                    msbf4_write(buf+0,  a0);
                    msbf4_write(buf+4,  a1);
                    msbf4_write(buf+8,  a2);
                    msbf4_write(buf+12, a3);
                }
            }

            // update block state
            if( (mode & AES_MIC)==0 || (mode & AES_MICNOAUX) ) {
                buf += 16;
                len -= 16;
            }
            mode |= AES_MICNOAUX;
        }
        return AESAUX[0];
}
#else
u4_t os_aes_internal (u1_t mode, xref2u1_t buf, u2_t len) {
    return 0;
}
#endif

u4_t os_aes (u1_t mode, xref2u1_t buf, u2_t len) {
    #if LMIC_AES_IMPLEMENTATION == 6
    aes128_key_schedule(key_schedule, AESkey);
    #elif LMIC_AES_IMPLEMENTATION == 7 && (AES_IMPLEMENTATION == 2 || AES_IMPLEMENTATION == 3)
    aesKeyExpand(AESkey, expanded_key);
    #endif

    u4_t start = micros();
    u4_t res = os_aes_internal(mode, buf, len);
    printf("AES mode %u of %u bytes took %u microseconds\n", (unsigned)mode, (unsigned)len, (unsigned)(micros() - start));
    return res;
}
