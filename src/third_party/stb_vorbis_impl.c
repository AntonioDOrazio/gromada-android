/* stb_vorbis (public domain / MIT, github.com/nothings/stb) compiled as its own C unit. */
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

void gromada_vorbis_info(stb_vorbis* f, unsigned* sample_rate, int* channels) {
    stb_vorbis_info info = stb_vorbis_get_info(f);
    *sample_rate = info.sample_rate;
    *channels = info.channels;
}
