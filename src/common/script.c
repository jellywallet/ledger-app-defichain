#include <stddef.h>
#include <stdint.h>
#include <limits.h>

#include "../common/bip32.h"
#include "../common/buffer.h"
#include "../common/read.h"
#include "../common/script.h"
#include "../common/segwit_addr.h"

#ifndef SKIP_FOR_CMOCKA
#include "../crypto.h"
#endif

#ifdef IS_DEFICHAIN
#include "dfi.h"
#endif

int get_script_type(const uint8_t script[], size_t script_len) {
    if (script_len == 25 && script[0] == OP_DUP && script[1] == OP_HASH160 && script[2] == 0x14 &&
        script[23] == OP_EQUALVERIFY && script[24] == OP_CHECKSIG) {
        return SCRIPT_TYPE_P2PKH;
    }

    if (script_len == 23 && script[0] == OP_HASH160 && script[1] == 0x14 &&
        script[22] == OP_EQUAL) {
        return SCRIPT_TYPE_P2SH;
    }

    if (script_len == 22 && script[0] == 0x00 && script[1] == 0x14) {
        return SCRIPT_TYPE_P2WPKH;
    }

    if (script_len == 34 && script[0] == OP_0 && script[1] == 0x20) {
        return SCRIPT_TYPE_P2WSH;
    }

    if (script_len == 34 && script[0] == OP_1 && script[1] == 0x20) {
        return SCRIPT_TYPE_P2TR;
    }

    // match if it is a potentially valid future segwit scriptPubKey as per BIP-0141
    if (script_len >= 4 && script_len <= 42 &&
        (script[0] == 0 || (script[0] >= OP_1 && script[0] <= OP_16))) {
        uint8_t push_len = script[1];
        if (script_len == 1 + 1 + push_len) {
            return SCRIPT_TYPE_UNKNOWN_SEGWIT;
        }
    }

    // unknown/invalid, or doesn't have an address
    return -1;
}

#ifndef SKIP_FOR_CMOCKA

// TODO: add unit tests
int get_script_address(const uint8_t script[],
                       size_t script_len,
                       const global_context_t *coin_config,
                       char *out,
                       size_t out_len) {
    int script_type = get_script_type(script, script_len);
    int addr_len;
    PRINTF("Parsing address type %d\n", script_type);

    switch (script_type) {
        case SCRIPT_TYPE_P2PKH:
        case SCRIPT_TYPE_P2SH: {
            int offset = (script_type == SCRIPT_TYPE_P2PKH) ? 3 : 2;
            int ver = (script_type == SCRIPT_TYPE_P2PKH) ? coin_config->p2pkh_version
                                                         : coin_config->p2sh_version;
            addr_len = base58_encode_address(script + offset, ver, out, out_len - 1);
            if (addr_len < 0) {
                return -1;
            }
            break;
        }
        case SCRIPT_TYPE_P2WPKH:
        case SCRIPT_TYPE_P2WSH:
        case SCRIPT_TYPE_P2TR:
        case SCRIPT_TYPE_UNKNOWN_SEGWIT: {
            uint8_t prog_len = script[1];  // length of the witness program

            // witness program version
            int version = (script[0] == 0 ? 0 : script[0] - 80);

            // make sure that the output buffer is long enough
            if (out_len < 73 + strlen(coin_config->native_segwit_prefix)) {
                return -1;
            }

            int ret = segwit_addr_encode(out,
                                         coin_config->native_segwit_prefix,
                                         version,
                                         script + 2,
                                         prog_len);
            if (ret != 1) {
                PRINTF("Should never happen\n");
                return -1;  // should never happen
            }

            addr_len = strlen(out);
            break;
        }
        default:
            return -1;
    }
    if (addr_len >= 0) {
        out[addr_len] = '\0';
    }
    return addr_len;
}

#endif

int format_opscript_script(const uint8_t script[],
                           size_t script_len,
                           char out[static MAX_OPRETURN_OUTPUT_DESC_SIZE],
                           bool *isDfiTx,
                           uint64_t *amount,
                           global_context_t *G_coin_config) {
    if (script_len == 0 || script[0] != OP_RETURN) {
        return -1;
    }

#ifdef IS_DEFICHAIN

    // script needs to be at least 5 char long DFI_TX_HEADER + len of the DFI script
    if (script_len >= 5) {
        PRINTF("CHECK IF DFI SCRIPT\n");
        int defiScriptLen = -1;
        int dfiHeaderPos = -1;
        if (script_len < OP_PUSHDATA1) {
            defiScriptLen = script[1];
            dfiHeaderPos = 2;
        } else if (script_len <= 0xff) {
            defiScriptLen = script[2];
            dfiHeaderPos = 3;
        }

        PRINTF("DFI Script Length: %d headerpos: %d\n", defiScriptLen, dfiHeaderPos);

        if (dfiHeaderPos > 0) {
            uint8_t dfiHeader[] = DFI_TX_HEADER;
            if (memcmp(&script[dfiHeaderPos], dfiHeader, 4) == 0) {
                PRINTF("PARSE DFI SCRIPT\n");
                *isDfiTx = true;
                return get_dfi_tx_type(&script[dfiHeaderPos + 4],
                                       defiScriptLen,
                                       out,
                                       MAX_OPRETURN_OUTPUT_DESC_SIZE,
                                       amount,
                                       G_coin_config);
            }
        }
    }
#endif
    strcpy(out, "OP_RETURN ");
    int out_ctr = 10;

    uint8_t opcode = script[1];  // the push opcode
    if (opcode > OP_16 || opcode == OP_RESERVED || opcode == OP_PUSHDATA2 ||
        opcode == OP_PUSHDATA4) {
        return -1;  // unsupported
    }

    int hex_offset = 1;
    size_t hex_length =
        0;  // if non-zero, `hex_length` bytes starting from script[hex_offset] must be hex-encoded

    if (opcode == OP_0) {
        if (script_len != 1 + 1) return -1;
        out[out_ctr++] = '0';
    } else if (opcode >= 1 && opcode <= 75) {
        hex_offset += 1;
        hex_length = opcode;

        if (script_len != 1 + 1 + hex_length) return -1;
    } else if (opcode == OP_PUSHDATA1) {
        // OP_RETURN OP_PUSHDATA1 <len:1-byte> <data:len bytes>
        hex_offset += 2;
        hex_length = script[2];

        if (script_len != 1 + 1 + 1 + hex_length || hex_length > 80) return -1;
    } else if (opcode == OP_1NEGATE) {
        if (script_len != 1 + 1) return -1;

        out[out_ctr++] = '-';
        out[out_ctr++] = '1';
    } else if (opcode >= OP_1 && opcode <= OP_16) {
        if (script_len != 1 + 1) return -1;

        // encode OP_1 to OP_16 as a decimal number
        uint8_t num = opcode - 0x50;
        if (num >= 10) {
            out[out_ctr++] = '0' + (num / 10);
        }
        out[out_ctr++] = '0' + (num % 10);
    } else {
        return -1;  // can never happen
    }

    if (hex_length > 0) {
        const char hex[] = "0123456789abcdef";

        out[out_ctr++] = '0';
        out[out_ctr++] = 'x';
        for (unsigned int i = 0; i < hex_length; i++) {
            uint8_t data = script[hex_offset + i];
            out[out_ctr++] = hex[data / 16];
            out[out_ctr++] = hex[data % 16];
        }
    }

    out[out_ctr++] = '\0';

    PRINTF("custom script %s\n", out);

    return out_ctr;
}