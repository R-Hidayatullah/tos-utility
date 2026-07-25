/* fsbcrypt.h - optional decryption layer for encrypted FSB files.
 *
 * FMOD's FSB codec reads plaintext (verified in fmodexD.dll:
 * CodecFSB5::headerReadCheck simply checks the "FSB5" magic, no decrypt step).
 * Encryption is a game-specific XOR layer applied on top; the key is NOT stored
 * in the file. Given the key, we decrypt in place and confirm success by the
 * appearance of a valid FSB magic.
 */
#ifndef FSB_CRYPT_H
#define FSB_CRYPT_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Try to decrypt `buf` in place using `key` (ASCII, or "0x..."/hex for raw bytes).
 * Attempts the common FSB XOR schemes and keeps the first that yields a valid
 * FSB magic. Returns the scheme index used (>0) on success, 0 if none worked
 * (buffer is left unchanged in that case). */
int fsb_try_decrypt(uint8_t *buf, size_t size, const char *key);

/* True if the first 4 bytes are a recognised FSB magic (FSB1..FSB5). */
int fsb_has_magic(const uint8_t *buf, size_t size);

#ifdef __cplusplus
}
#endif
#endif
