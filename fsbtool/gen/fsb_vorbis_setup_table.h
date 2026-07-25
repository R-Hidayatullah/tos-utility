/* Auto-generated companion header. See fsb_vorbis_setup_table.c */
#ifndef FSB_VORBIS_SETUP_TABLE_H
#define FSB_VORBIS_SETUP_TABLE_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned int         crc32;   /* key: FSB5 VORBISDATA crc32 */
    int                  length;  /* bytes of setup packet      */
    const unsigned char *data;    /* "\x05vorbis" + packed books */
} fsb_vorbis_setup;

extern const fsb_vorbis_setup fsb_vorbis_setups[];
extern const int              fsb_vorbis_setups_count;

/* Returns setup packet for a crc32, or NULL if not in the built-in table. */
const fsb_vorbis_setup *fsb_vorbis_setup_lookup(unsigned int crc32);

#ifdef __cplusplus
}
#endif
#endif
