/* Run: cc -Wall -Wextra -Werror -I. tests/callsid_test.c
 * siprec_metadata.c siprec_sb.c -o /tmp/callsid_test && /tmp/callsid_test */
#include "siprec_metadata.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    char id[SIPREC_METADATA_ID_LEN + 1];
    const char *valid[] = {
        "CA00112233445566778899aabbccddeeff",
        "CA00112233445566778899AABBCCDDEEFF",
        "00112233-4455-6677-8899-aabbccddeeff"
    };
    const char *invalid[] = {
        "XY00112233445566778899aabbccddeeff",
        "CA00112233-4455-6677-8899-aabbccddeeff",
        "CA00112233445566778899aabbccddeefg",
        "CA00112233445566778899aabbccddeef",
        "ca00112233445566778899aabbccddeeff"
    };
    for (size_t i = 0; i < sizeof(valid) / sizeof(*valid); ++i) {
        assert(siprec_metadata_uuid_to_id(valid[i], id) == 0);
        assert(strcmp(id, "ABEiM0RVZneImaq7zN3u/w==") == 0);
    }
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        assert(siprec_metadata_uuid_to_id(invalid[i], id) == -1);
        assert(*id == '\0');
    }
    return 0;
}
